// SPDX-License-Identifier: GPL-2.0

#include <linux/kobject.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/mm_inline.h>
#include <linux/pagevec.h>

#define DEFINE_MAX_SEQ(lruvec)						\
	unsigned long max_seq = READ_ONCE((lruvec)->lrugen.max_seq)

#define DEFINE_MIN_SEQ(lruvec)						\
	unsigned long min_seq[ANON_AND_FILE] = {			\
		READ_ONCE((lruvec)->lrugen.min_seq[LRU_GEN_ANON]),	\
		READ_ONCE((lruvec)->lrugen.min_seq[LRU_GEN_FILE]),	\
	}

static struct mem_cgroup *mem_cgroup_iter_simple(struct mem_cgroup *root,
				   struct mem_cgroup *prev)
{
	struct cgroup_subsys_state *css = NULL;
	struct mem_cgroup *memcg = NULL;
	struct mem_cgroup *pos = NULL;

	if (mem_cgroup_disabled())
		return NULL;

	if (!root)
		root = root_mem_cgroup;

	if (prev)
		pos = prev;

	rcu_read_lock();

	if (pos)
		css = &pos->css;

	for (;;) {
		css = css_next_descendant_pre(css, &root->css);
		if (!css) {
			/*
			 * Reclaimers share the hierarchy walk, and a
			 * new one might jump in right at the end of
			 * the hierarchy - make sure they see at least
			 * one group and restart from the beginning.
			 */
			if (!prev)
				continue;
			break;
		}

		/*
		 * Verify the css and acquire a reference.  The root
		 * is provided by the caller, so we know it's alive
		 * and kicking, and don't take an extra reference.
		 */
		memcg = mem_cgroup_from_css(css);

		if (css == &root->css)
			break;

		if (css_tryget(css))
			break;

		memcg = NULL;
	}

	rcu_read_unlock();
	if (prev && prev != root)
		css_put(&prev->css);

	return memcg;
}

int print_node_mglru(struct lruvec *lruvec, char *buf, int orig_pos)
{
	unsigned long seq;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	DEFINE_MAX_SEQ(lruvec);
	DEFINE_MIN_SEQ(lruvec);

	int print_pos = orig_pos;

	seq = min(min_seq[0], min_seq[1]);

	for (; seq <= max_seq; seq++) {
		int gen, type, zone;
		unsigned int msecs;

		gen = lru_gen_from_seq(seq);
		msecs = jiffies_to_msecs(jiffies - READ_ONCE(lrugen->timestamps[gen]));

		print_pos += snprintf(buf + print_pos, PAGE_SIZE - print_pos,
			" %10lu %10u", seq, msecs);

		for (type = 0; type < ANON_AND_FILE; type++) {
			long size = 0;

			if (seq < min_seq[type]) {
				print_pos += snprintf(buf + print_pos,
					PAGE_SIZE - print_pos, "         -0 ");
				continue;
			}

			for (zone = 0; zone < MAX_NR_ZONES; zone++)
				size += READ_ONCE(lrugen->nr_pages[gen][type][zone]);

			print_pos += snprintf(buf + print_pos,
				PAGE_SIZE - print_pos, " %10lu ", max(size, 0L));
		}

		print_pos += snprintf(buf + print_pos, PAGE_SIZE - print_pos, "\n");

	}

	return print_pos - orig_pos;
}

static ssize_t lru_gen_admin_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct lruvec *lruvec;
	struct mem_cgroup *memcg;

	char *path = kvmalloc(PATH_MAX, GFP_KERNEL);
	int buf_len = 0;

	if (!path)
		return -EINVAL;
	path[0] = 0;
	buf[0] = 0;
	memcg = mem_cgroup_iter_simple(NULL, NULL);
	do {
		int nid;

		for_each_node_state(nid, N_MEMORY) {
			lruvec = mem_cgroup_lruvec(memcg, NODE_DATA(nid));
			if (lruvec) {
				if (nid == first_memory_node) {
#ifdef CONFIG_MEMCG
					if (memcg)
						cgroup_path(memcg->css.cgroup, path, PATH_MAX);
					else
						path[0] = 0;
#endif
					buf_len += snprintf(buf + buf_len, PAGE_SIZE - buf_len,
						"memcg %5hu %s\n", mem_cgroup_id(memcg), path);
				}

				buf_len += snprintf(buf + buf_len, PAGE_SIZE - buf_len,
					" node %5d\n", nid);
				buf_len += print_node_mglru(lruvec, buf, buf_len);
			}
		}
	} while ((memcg = mem_cgroup_iter_simple(NULL, memcg)));

	if (buf_len >= PAGE_SIZE)
		buf_len = PAGE_SIZE - 1;
	buf[buf_len] = 0;

	kvfree(path);

	return buf_len;
}

static struct kobj_attribute lru_gen_admin_attr = __ATTR_RO(lru_gen_admin);

static int __init init_mglru_sysfs(void)
{
	BUILD_BUG_ON(MIN_NR_GENS + 1 >= MAX_NR_GENS);
	BUILD_BUG_ON(BIT(LRU_GEN_WIDTH) <= MAX_NR_GENS);

	if (sysfs_create_file(mm_kobj, &lru_gen_admin_attr.attr))
		pr_err("lru_gen: failed to create sysfs group\n");

	return 0;
};
late_initcall(init_mglru_sysfs);

MODULE_LICENSE("GPL");
