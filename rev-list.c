#include "cache.h"
#include "refs.h"
#include "tag.h"
#include "commit.h"
#include "tree.h"
#include "blob.h"
#include "tree-walk.h"
#include "revision.h"

/* bits #0-5 in revision.h */

#define COUNTED		(1u<<6)

static const char rev_list_usage[] =
"git-rev-list [OPTION] <commit-id>... [ -- paths... ]\n"
"  limiting output:\n"
"    --max-count=nr\n"
"    --max-age=epoch\n"
"    --min-age=epoch\n"
"    --sparse\n"
"    --no-merges\n"
"    --remove-empty\n"
"    --all\n"
"  ordering output:\n"
"    --topo-order\n"
"    --date-order\n"
"  formatting output:\n"
"    --parents\n"
"    --objects | --objects-edge\n"
"    --unpacked\n"
"    --header | --pretty\n"
"    --abbrev=nr | --no-abbrev\n"
"    --abbrev-commit\n"
"  special purpose:\n"
"    --bisect"
;

struct rev_info revs;

static int bisect_list = 0;
static int verbose_header = 0;
static int abbrev = DEFAULT_ABBREV;
static int abbrev_commit = 0;
static int show_timestamp = 0;
static int hdr_termination = 0;
static const char *commit_prefix = "";
static enum cmit_fmt commit_format = CMIT_FMT_RAW;

static void show_commit(struct commit *commit)
{
	if (show_timestamp)
		printf("%lu ", commit->date);
	if (commit_prefix[0])
		fputs(commit_prefix, stdout);
	if (commit->object.flags & BOUNDARY)
		putchar('-');
	if (abbrev_commit && abbrev)
		fputs(find_unique_abbrev(commit->object.sha1, abbrev), stdout);
	else
		fputs(sha1_to_hex(commit->object.sha1), stdout);
	if (revs.parents) {
		struct commit_list *parents = commit->parents;
		while (parents) {
			struct object *o = &(parents->item->object);
			parents = parents->next;
			if (o->flags & TMP_MARK)
				continue;
			printf(" %s", sha1_to_hex(o->sha1));
			o->flags |= TMP_MARK;
		}
		/* TMP_MARK is a general purpose flag that can
		 * be used locally, but the user should clean
		 * things up after it is done with them.
		 */
		for (parents = commit->parents;
		     parents;
		     parents = parents->next)
			parents->item->object.flags &= ~TMP_MARK;
	}
	if (commit_format == CMIT_FMT_ONELINE)
		putchar(' ');
	else
		putchar('\n');

	if (verbose_header) {
		static char pretty_header[16384];
		pretty_print_commit(commit_format, commit, ~0, pretty_header, sizeof(pretty_header), abbrev);
		printf("%s%c", pretty_header, hdr_termination);
	}
	fflush(stdout);
}

static struct object_list **process_blob(struct blob *blob,
					 struct object_list **p,
					 struct name_path *path,
					 const char *name)
{
	struct object *obj = &blob->object;

	if (!revs.blob_objects)
		return p;
	if (obj->flags & (UNINTERESTING | SEEN))
		return p;
	obj->flags |= SEEN;
	return add_object(obj, p, path, name);
}

static struct object_list **process_tree(struct tree *tree,
					 struct object_list **p,
					 struct name_path *path,
					 const char *name)
{
	struct object *obj = &tree->object;
	struct tree_entry_list *entry;
	struct name_path me;

	if (!revs.tree_objects)
		return p;
	if (obj->flags & (UNINTERESTING | SEEN))
		return p;
	if (parse_tree(tree) < 0)
		die("bad tree object %s", sha1_to_hex(obj->sha1));
	obj->flags |= SEEN;
	p = add_object(obj, p, path, name);
	me.up = path;
	me.elem = name;
	me.elem_len = strlen(name);
	entry = tree->entries;
	tree->entries = NULL;
	while (entry) {
		struct tree_entry_list *next = entry->next;
		if (entry->directory)
			p = process_tree(entry->item.tree, p, &me, entry->name);
		else
			p = process_blob(entry->item.blob, p, &me, entry->name);
		free(entry);
		entry = next;
	}
	return p;
}

static void show_commit_list(struct rev_info *revs)
{
	struct commit *commit;
	struct object_list *objects = NULL, **p = &objects, *pending;

	while ((commit = get_revision(revs)) != NULL) {
		p = process_tree(commit->tree, p, NULL, "");
		show_commit(commit);
	}
	for (pending = revs->pending_objects; pending; pending = pending->next) {
		struct object *obj = pending->item;
		const char *name = pending->name;
		if (obj->flags & (UNINTERESTING | SEEN))
			continue;
		if (obj->type == tag_type) {
			obj->flags |= SEEN;
			p = add_object(obj, p, NULL, name);
			continue;
		}
		if (obj->type == tree_type) {
			p = process_tree((struct tree *)obj, p, NULL, name);
			continue;
		}
		if (obj->type == blob_type) {
			p = process_blob((struct blob *)obj, p, NULL, name);
			continue;
		}
		die("unknown pending object %s (%s)", sha1_to_hex(obj->sha1), name);
	}
	while (objects) {
		/* An object with name "foo\n0000000..." can be used to
		 * confuse downstream git-pack-objects very badly.
		 */
		const char *ep = strchr(objects->name, '\n');
		if (ep) {
			printf("%s %.*s\n", sha1_to_hex(objects->item->sha1),
			       (int) (ep - objects->name),
			       objects->name);
		}
		else
			printf("%s %s\n", sha1_to_hex(objects->item->sha1), objects->name);
		objects = objects->next;
	}
}

/*
 * This is a truly stupid algorithm, but it's only
 * used for bisection, and we just don't care enough.
 *
 * We care just barely enough to avoid recursing for
 * non-merge entries.
 */
static int count_distance(struct commit_list *entry)
{
	int nr = 0;

	while (entry) {
		struct commit *commit = entry->item;
		struct commit_list *p;

		if (commit->object.flags & (UNINTERESTING | COUNTED))
			break;
		if (!revs.prune_fn || (commit->object.flags & TREECHANGE))
			nr++;
		commit->object.flags |= COUNTED;
		p = commit->parents;
		entry = p;
		if (p) {
			p = p->next;
			while (p) {
				nr += count_distance(p);
				p = p->next;
			}
		}
	}

	return nr;
}

static void clear_distance(struct commit_list *list)
{
	while (list) {
		struct commit *commit = list->item;
		commit->object.flags &= ~COUNTED;
		list = list->next;
	}
}

static struct commit_list *find_bisection(struct commit_list *list)
{
	int nr, closest;
	struct commit_list *p, *best;

	nr = 0;
	p = list;
	while (p) {
		if (!revs.prune_fn || (p->item->object.flags & TREECHANGE))
			nr++;
		p = p->next;
	}
	closest = 0;
	best = list;

	for (p = list; p; p = p->next) {
		int distance;

		if (revs.prune_fn && !(p->item->object.flags & TREECHANGE))
			continue;

		distance = count_distance(p);
		clear_distance(list);
		if (nr - distance < distance)
			distance = nr - distance;
		if (distance > closest) {
			best = p;
			closest = distance;
		}
	}
	if (best)
		best->next = NULL;
	return best;
}

static void mark_edge_parents_uninteresting(struct commit *commit)
{
	struct commit_list *parents;

	for (parents = commit->parents; parents; parents = parents->next) {
		struct commit *parent = parents->item;
		if (!(parent->object.flags & UNINTERESTING))
			continue;
		mark_tree_uninteresting(parent->tree);
		if (revs.edge_hint && !(parent->object.flags & SHOWN)) {
			parent->object.flags |= SHOWN;
			printf("-%s\n", sha1_to_hex(parent->object.sha1));
		}
	}
}

static void mark_edges_uninteresting(struct commit_list *list)
{
	for ( ; list; list = list->next) {
		struct commit *commit = list->item;

		if (commit->object.flags & UNINTERESTING) {
			mark_tree_uninteresting(commit->tree);
			continue;
		}
		mark_edge_parents_uninteresting(commit);
	}
}

int main(int argc, const char **argv)
{
	struct commit_list *list;
	int i;

	argc = setup_revisions(argc, argv, &revs, NULL);

	for (i = 1 ; i < argc; i++) {
		const char *arg = argv[i];

		/* accept -<digit>, like traditilnal "head" */
		if ((*arg == '-') && isdigit(arg[1])) {
			revs.max_count = atoi(arg + 1);
			continue;
		}
		if (!strcmp(arg, "-n")) {
			if (++i >= argc)
				die("-n requires an argument");
			revs.max_count = atoi(argv[i]);
			continue;
		}
		if (!strncmp(arg,"-n",2)) {
			revs.max_count = atoi(arg + 2);
			continue;
		}
		if (!strcmp(arg, "--header")) {
			verbose_header = 1;
			continue;
		}
		if (!strcmp(arg, "--no-abbrev")) {
			abbrev = 0;
			continue;
		}
		if (!strcmp(arg, "--abbrev")) {
			abbrev = DEFAULT_ABBREV;
			continue;
		}
		if (!strcmp(arg, "--abbrev-commit")) {
			abbrev_commit = 1;
			continue;
		}
		if (!strncmp(arg, "--abbrev=", 9)) {
			abbrev = strtoul(arg + 9, NULL, 10);
			if (abbrev && abbrev < MINIMUM_ABBREV)
				abbrev = MINIMUM_ABBREV;
			else if (40 < abbrev)
				abbrev = 40;
			continue;
		}
		if (!strncmp(arg, "--pretty", 8)) {
			commit_format = get_commit_format(arg+8);
			verbose_header = 1;
			hdr_termination = '\n';
			if (commit_format == CMIT_FMT_ONELINE)
				commit_prefix = "";
			else
				commit_prefix = "commit ";
			continue;
		}
		if (!strcmp(arg, "--timestamp")) {
			show_timestamp = 1;
			continue;
		}
		if (!strcmp(arg, "--bisect")) {
			bisect_list = 1;
			continue;
		}
		usage(rev_list_usage);

	}

	list = revs.commits;

	if (!list &&
	    (!(revs.tag_objects||revs.tree_objects||revs.blob_objects) && !revs.pending_objects))
		usage(rev_list_usage);

	save_commit_buffer = verbose_header;
	track_object_refs = 0;

	prepare_revision_walk(&revs);
	if (revs.tree_objects)
		mark_edges_uninteresting(revs.commits);

	if (bisect_list)
		revs.commits = find_bisection(revs.commits);

	show_commit_list(&revs);

	return 0;
}
