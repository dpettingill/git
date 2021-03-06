/*
 * "git reset" builtin command
 *
 * Copyright (c) 2007 Carlos Rica
 *
 * Based on git-reset.sh, which is
 *
 * Copyright (c) 2005, 2006 Linus Torvalds and Junio C Hamano
 */
#include "builtin.h"
#include "config.h"
#include "lockfile.h"
#include "tag.h"
#include "object.h"
#include "pretty.h"
#include "run-command.h"
#include "refs.h"
#include "diff.h"
#include "diffcore.h"
#include "tree.h"
#include "branch.h"
#include "parse-options.h"
#include "unpack-trees.h"
#include "cache-tree.h"
#include "submodule.h"
#include "submodule-config.h"
#include "strbuf.h"
#include "quote.h"

static const char * const git_reset_usage[] = {
	N_("git reset [--mixed | --soft | --hard | --merge | --keep] [-q] [<commit>]"),
	N_("git reset [-q] [<tree-ish>] [--] <paths>..."),
	N_("EXPERIMENTAL: git reset [-q] [--stdin [-z]] [<tree-ish>]"),
	N_("git reset --patch [<tree-ish>] [--] [<paths>...]"),
	NULL
};

enum reset_type { MIXED, SOFT, HARD, MERGE, KEEP, NONE };
static const char *reset_type_names[] = {
	N_("mixed"), N_("soft"), N_("hard"), N_("merge"), N_("keep"), NULL
};

static inline int is_merge(void)
{
	return !access(git_path_merge_head(the_repository), F_OK);
}

static int reset_index(const struct object_id *oid, int reset_type, int quiet)
{
	int i, nr = 0;
	struct tree_desc desc[2];
	struct tree *tree;
	struct unpack_trees_options opts;
	int ret = -1;

	memset(&opts, 0, sizeof(opts));
	opts.head_idx = 1;
	opts.src_index = &the_index;
	opts.dst_index = &the_index;
	opts.fn = oneway_merge;
	opts.merge = 1;
	if (!quiet)
		opts.verbose_update = 1;
	switch (reset_type) {
	case KEEP:
	case MERGE:
		opts.update = 1;
		break;
	case HARD:
		opts.update = 1;
		/* fallthrough */
	default:
		opts.reset = 1;
	}

	read_cache_unmerged();

	if (reset_type == KEEP) {
		struct object_id head_oid;
		if (get_oid("HEAD", &head_oid))
			return error(_("You do not have a valid HEAD."));
		if (!fill_tree_descriptor(desc + nr, &head_oid))
			return error(_("Failed to find tree of HEAD."));
		nr++;
		opts.fn = twoway_merge;
	}

	if (!fill_tree_descriptor(desc + nr, oid)) {
		error(_("Failed to find tree of %s."), oid_to_hex(oid));
		goto out;
	}
	nr++;

	if (unpack_trees(nr, desc, &opts))
		goto out;

	if (reset_type == MIXED || reset_type == HARD) {
		tree = parse_tree_indirect(oid);
		prime_cache_tree(&the_index, tree);
	}

	ret = 0;

out:
	for (i = 0; i < nr; i++)
		free((void *)desc[i].buffer);
	return ret;
}

static void print_new_head_line(struct commit *commit)
{
	struct strbuf buf = STRBUF_INIT;

	printf(_("HEAD is now at %s"),
		find_unique_abbrev(&commit->object.oid, DEFAULT_ABBREV));

	pp_commit_easy(CMIT_FMT_ONELINE, commit, &buf);
	if (buf.len > 0)
		printf(" %s", buf.buf);
	putchar('\n');
	strbuf_release(&buf);
}

static void update_index_from_diff(struct diff_queue_struct *q,
		struct diff_options *opt, void *data)
{
	int i;
	int intent_to_add = *(int *)data;

	for (i = 0; i < q->nr; i++) {
		struct diff_filespec *one = q->queue[i]->one;
		int is_missing = !(one->mode && !is_null_oid(&one->oid));
		struct cache_entry *ce;

		if (is_missing && !intent_to_add) {
			remove_file_from_cache(one->path);
			continue;
		}

		ce = make_cache_entry(&the_index, one->mode, &one->oid, one->path,
				      0, 0);
		if (!ce)
			die(_("make_cache_entry failed for path '%s'"),
			    one->path);
		if (is_missing) {
			ce->ce_flags |= CE_INTENT_TO_ADD;
			set_object_name_for_intent_to_add_entry(ce);
		}
		add_cache_entry(ce, ADD_CACHE_OK_TO_ADD | ADD_CACHE_OK_TO_REPLACE);
	}
}

static int read_from_tree(const struct pathspec *pathspec,
			  struct object_id *tree_oid,
			  int intent_to_add)
{
	struct diff_options opt;

	memset(&opt, 0, sizeof(opt));
	copy_pathspec(&opt.pathspec, pathspec);
	opt.output_format = DIFF_FORMAT_CALLBACK;
	opt.format_callback = update_index_from_diff;
	opt.format_callback_data = &intent_to_add;
	opt.flags.override_submodule_config = 1;

	if (do_diff_cache(tree_oid, &opt))
		return 1;
	diffcore_std(&opt);
	diff_flush(&opt);
	clear_pathspec(&opt.pathspec);

	return 0;
}

static void set_reflog_message(struct strbuf *sb, const char *action,
			       const char *rev)
{
	const char *rla = getenv("GIT_REFLOG_ACTION");

	strbuf_reset(sb);
	if (rla)
		strbuf_addf(sb, "%s: %s", rla, action);
	else if (rev)
		strbuf_addf(sb, "reset: moving to %s", rev);
	else
		strbuf_addf(sb, "reset: %s", action);
}

static void die_if_unmerged_cache(int reset_type)
{
	if (is_merge() || unmerged_cache())
		die(_("Cannot do a %s reset in the middle of a merge."),
		    _(reset_type_names[reset_type]));

}

static void parse_args(struct pathspec *pathspec,
		       const char **argv, const char *prefix,
		       int patch_mode,
		       const char **rev_ret)
{
	const char *rev = "HEAD";
	struct object_id unused;
	/*
	 * Possible arguments are:
	 *
	 * git reset [-opts] [<rev>]
	 * git reset [-opts] <tree> [<paths>...]
	 * git reset [-opts] <tree> -- [<paths>...]
	 * git reset [-opts] -- [<paths>...]
	 * git reset [-opts] <paths>...
	 *
	 * At this point, argv points immediately after [-opts].
	 */

	if (argv[0]) {
		if (!strcmp(argv[0], "--")) {
			argv++; /* reset to HEAD, possibly with paths */
		} else if (argv[1] && !strcmp(argv[1], "--")) {
			rev = argv[0];
			argv += 2;
		}
		/*
		 * Otherwise, argv[0] could be either <rev> or <paths> and
		 * has to be unambiguous. If there is a single argument, it
		 * can not be a tree
		 */
		else if ((!argv[1] && !get_oid_committish(argv[0], &unused)) ||
			 (argv[1] && !get_oid_treeish(argv[0], &unused))) {
			/*
			 * Ok, argv[0] looks like a commit/tree; it should not
			 * be a filename.
			 */
			verify_non_filename(prefix, argv[0]);
			rev = *argv++;
		} else {
			/* Otherwise we treat this as a filename */
			verify_filename(prefix, argv[0], 1);
		}
	}
	*rev_ret = rev;

	if (read_cache() < 0)
		die(_("index file corrupt"));

	parse_pathspec(pathspec, 0,
		       PATHSPEC_PREFER_FULL |
		       (patch_mode ? PATHSPEC_PREFIX_ORIGIN : 0),
		       prefix, argv);
}

static int reset_refs(const char *rev, const struct object_id *oid)
{
	int update_ref_status;
	struct strbuf msg = STRBUF_INIT;
	struct object_id *orig = NULL, oid_orig,
		*old_orig = NULL, oid_old_orig;

	if (!get_oid("ORIG_HEAD", &oid_old_orig))
		old_orig = &oid_old_orig;
	if (!get_oid("HEAD", &oid_orig)) {
		orig = &oid_orig;
		set_reflog_message(&msg, "updating ORIG_HEAD", NULL);
		update_ref(msg.buf, "ORIG_HEAD", orig, old_orig, 0,
			   UPDATE_REFS_MSG_ON_ERR);
	} else if (old_orig)
		delete_ref(NULL, "ORIG_HEAD", old_orig, 0);
	set_reflog_message(&msg, "updating HEAD", rev);
	update_ref_status = update_ref(msg.buf, "HEAD", oid, orig, 0,
				       UPDATE_REFS_MSG_ON_ERR);
	strbuf_release(&msg);
	return update_ref_status;
}

static int git_reset_config(const char *var, const char *value, void *cb)
{
	if (!strcmp(var, "submodule.recurse"))
		return git_default_submodule_config(var, value, cb);

	return git_default_config(var, value, cb);
}

int cmd_reset(int argc, const char **argv, const char *prefix)
{
	int reset_type = NONE, update_ref_status = 0, quiet = 0;
	int patch_mode = 0, nul_term_line = 0, read_from_stdin = 0, unborn;
	char **stdin_paths = NULL;
	int stdin_nr = 0, stdin_alloc = 0;
	const char *rev;
	struct object_id oid;
	struct pathspec pathspec;
	int intent_to_add = 0;
	const struct option options[] = {
		OPT__QUIET(&quiet, N_("be quiet, only report errors")),
		OPT_SET_INT(0, "mixed", &reset_type,
						N_("reset HEAD and index"), MIXED),
		OPT_SET_INT(0, "soft", &reset_type, N_("reset only HEAD"), SOFT),
		OPT_SET_INT(0, "hard", &reset_type,
				N_("reset HEAD, index and working tree"), HARD),
		OPT_SET_INT(0, "merge", &reset_type,
				N_("reset HEAD, index and working tree"), MERGE),
		OPT_SET_INT(0, "keep", &reset_type,
				N_("reset HEAD but keep local changes"), KEEP),
		{ OPTION_CALLBACK, 0, "recurse-submodules", NULL,
			    "reset", "control recursive updating of submodules",
			    PARSE_OPT_OPTARG, option_parse_recurse_submodules_worktree_updater },
		OPT_BOOL('p', "patch", &patch_mode, N_("select hunks interactively")),
		OPT_BOOL('N', "intent-to-add", &intent_to_add,
				N_("record only the fact that removed paths will be added later")),
		OPT_BOOL('z', NULL, &nul_term_line,
			N_("EXPERIMENTAL: paths are separated with NUL character")),
		OPT_BOOL(0, "stdin", &read_from_stdin,
				N_("EXPERIMENTAL: read paths from <stdin>")),
		OPT_END()
	};

	git_config(git_reset_config, NULL);

	argc = parse_options(argc, argv, prefix, options, git_reset_usage,
						PARSE_OPT_KEEP_DASHDASH);
	parse_args(&pathspec, argv, prefix, patch_mode, &rev);

	if (read_from_stdin) {
		strbuf_getline_fn getline_fn = nul_term_line ?
			strbuf_getline_nul : strbuf_getline_lf;
		int flags = PATHSPEC_PREFER_FULL;
		struct strbuf buf = STRBUF_INIT;
		struct strbuf unquoted = STRBUF_INIT;

		if (patch_mode)
			die(_("--stdin is incompatible with --patch"));

		if (pathspec.nr)
			die(_("--stdin is incompatible with path arguments"));

		while (getline_fn(&buf, stdin) != EOF) {
			if (!nul_term_line && buf.buf[0] == '"') {
				strbuf_reset(&unquoted);
				if (unquote_c_style(&unquoted, buf.buf, NULL))
					die(_("line is badly quoted"));
				strbuf_swap(&buf, &unquoted);
			}
			ALLOC_GROW(stdin_paths, stdin_nr + 1, stdin_alloc);
			stdin_paths[stdin_nr++] = xstrdup(buf.buf);
			strbuf_reset(&buf);
		}
		strbuf_release(&unquoted);
		strbuf_release(&buf);

		ALLOC_GROW(stdin_paths, stdin_nr + 1, stdin_alloc);
		stdin_paths[stdin_nr++] = NULL;
		flags |= PATHSPEC_LITERAL_PATH;
		parse_pathspec(&pathspec, 0, flags, prefix,
			       (const char **)stdin_paths);

	} else if (nul_term_line)
		die(_("-z requires --stdin"));

	unborn = !strcmp(rev, "HEAD") && get_oid("HEAD", &oid);
	if (unborn) {
		/* reset on unborn branch: treat as reset to empty tree */
		oidcpy(&oid, the_hash_algo->empty_tree);
	} else if (!pathspec.nr) {
		struct commit *commit;
		if (get_oid_committish(rev, &oid))
			die(_("Failed to resolve '%s' as a valid revision."), rev);
		commit = lookup_commit_reference(the_repository, &oid);
		if (!commit)
			die(_("Could not parse object '%s'."), rev);
		oidcpy(&oid, &commit->object.oid);
	} else {
		struct tree *tree;
		if (get_oid_treeish(rev, &oid))
			die(_("Failed to resolve '%s' as a valid tree."), rev);
		tree = parse_tree_indirect(&oid);
		if (!tree)
			die(_("Could not parse object '%s'."), rev);
		oidcpy(&oid, &tree->object.oid);
	}

	if (patch_mode) {
		if (reset_type != NONE)
			die(_("--patch is incompatible with --{hard,mixed,soft}"));
		return run_add_interactive(rev, "--patch=reset", &pathspec);
	}

	/* git reset tree [--] paths... can be used to
	 * load chosen paths from the tree into the index without
	 * affecting the working tree nor HEAD. */
	if (pathspec.nr) {
		if (reset_type == MIXED)
			warning(_("--mixed with paths is deprecated; use 'git reset -- <paths>' instead."));
		else if (reset_type != NONE)
			die(_("Cannot do %s reset with paths."),
					_(reset_type_names[reset_type]));
	}
	if (reset_type == NONE)
		reset_type = MIXED; /* by default */

	if (reset_type != SOFT && (reset_type != MIXED || get_git_work_tree()))
		setup_work_tree();

	if (reset_type == MIXED && is_bare_repository())
		die(_("%s reset is not allowed in a bare repository"),
		    _(reset_type_names[reset_type]));

	if (intent_to_add && reset_type != MIXED)
		die(_("-N can only be used with --mixed"));

	/* Soft reset does not touch the index file nor the working tree
	 * at all, but requires them in a good order.  Other resets reset
	 * the index file to the tree object we are switching to. */
	if (reset_type == SOFT || reset_type == KEEP)
		die_if_unmerged_cache(reset_type);

	if (reset_type != SOFT) {
		struct lock_file lock = LOCK_INIT;
		hold_locked_index(&lock, LOCK_DIE_ON_ERROR);
		if (reset_type == MIXED) {
			int flags = quiet ? REFRESH_QUIET : REFRESH_IN_PORCELAIN;
			if (read_from_tree(&pathspec, &oid, intent_to_add))
				return 1;
			if (get_git_work_tree())
				refresh_index(&the_index, flags, NULL, NULL,
					      _("Unstaged changes after reset:"));
		} else {
			int err = reset_index(&oid, reset_type, quiet);
			if (reset_type == KEEP && !err)
				err = reset_index(&oid, MIXED, quiet);
			if (err)
				die(_("Could not reset index file to revision '%s'."), rev);
		}

		if (write_locked_index(&the_index, &lock, COMMIT_LOCK))
			die(_("Could not write new index file."));
	}

	if (!pathspec.nr && !unborn) {
		/* Any resets without paths update HEAD to the head being
		 * switched to, saving the previous head in ORIG_HEAD before. */
		update_ref_status = reset_refs(rev, &oid);

		if (reset_type == HARD && !update_ref_status && !quiet)
			print_new_head_line(lookup_commit_reference(the_repository, &oid));
	}
	if (!pathspec.nr)
		remove_branch_state();

	if (stdin_paths) {
		while (stdin_nr)
			free(stdin_paths[--stdin_nr]);
		free(stdin_paths);
	}

	return update_ref_status;
}
