#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/conf.h>
#include <openssl/evp.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <list>
#include <map>
#include <gtk/gtk.h>

#define PROG_NAME "Pollux"
#define PROG_NAME_DBUS "org.gtk.pollux"
#define POLLUX_BUILD "2.1.0"
#define POLLUX_LOGO "./pollux_logo2.svg"
#define PROG_COMMENTS "A file system scanner for duplicate files"
#define PROG_AUTHORS { "Gary Hannah", (gchar *)NULL }
#define PROG_WEBSITE "https://127.0.0.1:80/?real=false&amp;fake=true"
#define PROG_LICENCE "© Licenced under GNU Library GPLv2"

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#ifndef PATH_MAX
# define PATH_MAX 1024
#endif

guint DIGEST_SIZE = 0;
EVP_MD *__default_digest = (EVP_MD *)EVP_md5();

static gchar *get_file_digest(gchar *) __nonnull((1)) __wur;

static struct sigaction sigint_old;
static struct sigaction sigint_new;
static sigjmp_buf __root_env__;

typedef void (*gcallback_t)(GtkWidget *, gpointer);

void on_digest_select(GtkWidget *, gpointer);
void on_option_select(GtkWidget *, gpointer);
void create_window(void);
void create_menu_bar(void);

static GtkApplication *app;

#define WIN_DEFAULT_WIDTH 500
#define WIN_DEFAULT_HEIGHT 400
static GtkWidget *window;

#define MAIN_GRID_ROW_SPACING 0
#define MAIN_GRID_COL_SPACING 8
static GtkWidget *grid;

static GtkWidget *menu_bar;
static GtkWidget *file_menu;
static GtkWidget *item_file;
static GtkWidget *item_file_quit;
static GtkWidget *options_menu;
static GtkWidget *digests_menu;
static GtkWidget *item_options;
static GtkWidget *item_digests;
static GtkWidget item_digest_md5;
static GtkWidget item_digest_sha256;
static GtkWidget item_digest_sha512;

static GtkWidget *stats_nr_files;
static GtkWidget *sep1;
static GtkWidget *stats_nr_dups;
static GtkWidget *sep2;
static GtkWidget *stats_nr_bytes_total;
static GtkWidget *sep3;
static GtkWidget *stats_nr_bytes_wasted;
static GtkWidget *sep4;

static GtkListStore *stats_list;
static GtkWidget *stats_view;
static GtkCellRenderer *stats_renderer;

#define INC_NR_FILES() (++_stats.nr_files)
#define INC_NR_DUPS() (++_stats.nr_dups)
#define INC_BYTES_TOTAL(b) (_stats.nr_bytes_total += (b))
#define INC_BYTES_WASTED(b) (_stats.nr_bytes_wasted += (b))

struct Stats
{
	guint nr_files;
	guint nr_dups;
	guint nr_bytes_total;
	guint nr_bytes_wasted;
};

static struct Stats _stats = {0};

/* runtime options */
static GtkWidget *options_box;
static GtkWidget no_hidden;
static GtkWidget delete_all;

#define PROG_ICON_WIDTH 178
#define PROG_ICON_HEIGHT 178
static GdkPixbuf *icon_pixbuf;

#define PROG_ICON_SMALL_WIDTH 30
#define PROG_ICON_SMALL_HEIGHT 30
static GdkPixbuf *icon_pixbuf_small;

static GtkWidget *list_box;

static GtkWidget *label_choose_dir;
static GtkWidget *button_choose_dir;
static GtkWidget *button_start_scan;
static GtkWidget *image;
static GtkWidget *image_small;

static GtkWidget *status_bar;

#define SCROLLING_WINDOW_WIDTH 1650
#define SCROLLING_WINDOW_HEIGHT 600
static GtkWidget *results_window;
static GtkWidget *scrolling;
static GtkTreeStore *store;
static GtkWidget *view;
static GtkCellRenderer *renderer;
static GtkCellRenderer *toggle_renderer;
static GtkWidget *check_button;

static GList *list_digests;

enum
{
	COL_SELECT = 0,
	COL_IS_ALL,
	COL_PATH,
	COL_SIZE,
	COL_TIME_CREATED,
	COL_TIME_MODIFIED,
	NR_COLUMNS
};

struct Result_Columns
{
	const gchar *name;
};

struct Result_Columns result_columns[NR_COLUMNS] =
{
	{ "  Select  " },
	{ " " },
	{ "File" },
	{ "       Size       " },
	{ "       Created       " },
	{ "       Modified       " }
};

#define OPT_NO_HIDDEN 0x1
#define OPT_DELETE_ALL 0x2

enum
{
	OPTION_NO_HIDDEN = 0,
	OPTION_DELETE_ALL,
	NR_OPTIONS
};

struct Runtime_Option
{
	GtkWidget *widget;
	gint option;
	const gchar *name;
	gcallback_t func;
};

struct Runtime_Option options[NR_OPTIONS] =
{
	{ &no_hidden, OPTION_NO_HIDDEN, "Ignore hidden files", on_option_select },
	{ &delete_all, OPTION_DELETE_ALL, "Delete all (no prompt)", on_option_select }
};

enum
{
	DIGEST_MD5,
	DIGEST_SHA256,
	DIGEST_SHA512,
	NR_DIGESTS
};

struct Digest
{
	GtkWidget *item;
	gint type;
	const gchar *name;
	gcallback_t func;
};

static struct Digest menu_digests[NR_DIGESTS] =
{
	{ &item_digest_md5, DIGEST_MD5, (const gchar *)"MD5", on_digest_select },
	{ &item_digest_sha256, DIGEST_SHA256, (const gchar *)"SHA256", on_digest_select },
	{ &item_digest_sha512, DIGEST_SHA512, (const gchar *)"SHA512", on_digest_select }
};

struct POLLUX_CTX
{
	gchar start_at[PATH_MAX];
	gint digest_type;
	guint runtime_options;
	gboolean scanning;
	EVP_MD *digest_func;
};

#define option_set(o) (CTX.runtime_options & (o))
#define set_option(o) (CTX.runtime_options |= (o))
#define unset_option(o) (CTX.runtime_options &= ~(o))

#define __DEFAULT_DIGEST DIGEST_MD5

struct POLLUX_CTX CTX =
{
	"",
	__DEFAULT_DIGEST,
	~(1u),
	FALSE,
	(EVP_MD *)NULL
};

class fNode
{
	public:

	gchar *digest;
	gchar *name;
	gsize size;
	fNode *left;
	fNode *right;
	fNode *parent;

	fNode();
	~fNode();

	bool has_digest(void)
	{
		return this->have_digest;
	}

	void got_digest(bool tvalue)
	{
		this->have_digest = tvalue;
	}

	bool been_added(void)
	{
		return this->added;
	}

	void set_added(bool tvalue)
	{
		this->added = tvalue;
	}

	void add_to_bucket(fNode& node)
	{
		this->bucket.push_back(node);
		this->nr_bucket += 1;
	}

	bool bucket_contains(gchar *digest, gsize size)
	{
		for (std::list<fNode>::iterator it = this->bucket.begin(); it != this->bucket.end(); ++it)
		{
			if (!memcmp(digest, it->digest, size))
			{
				return true;
			}
		}

		return false;
	}

	gint nr_items_bucket(void)
	{
		return this->nr_bucket;
	}

	private:

	bool have_digest;
	bool added;
	std::list<fNode> bucket;
	gsize nr_bucket;
};

fNode::fNode(void)
{
	this->digest = NULL;
	this->name = NULL;
	this->size = 0;
	this->left = NULL;
	this->right = NULL;
	this->parent = NULL;
	this->have_digest = false;
	this->added = false;
	this->nr_bucket = 0;
}

fNode::~fNode(void)
{
	if (this->digest)
		free(this->digest);

	if (this->name)
		free(this->name);
}

/*
 * Duplicate file node.
 */
class dNode
{
	public:

	gchar *name;

	dNode();
};

dNode::dNode(void)
{
	this->name = NULL;
}

/*
 * Now using a map of type std::map<gchar *,std::list<dNode> >
 *
 * Gives O(log(N)) search time to find the linked list of
 * files that match the digest (which is the key).
 */
#if 0
/*
 * Linked list of digests, containing
 * a head pointer to linked list of
 * all the files that have this digest.
 */
class dList
{
	public:

	gsize nr_nodes;
	std::list<dNode> files;

	dList();

	gint get_nr_items(void)
	{
		return this->nr_nodes;
	}

	gint add_node(dNode node)
	{
		this->files.push_back(node);
		this->nr_nodes += 1;
	}
};

dList::dList(void)
{
	this->digest = NULL;
	this->nr_nodes = 0;
}
#endif

class fTree
{
	public:

	gsize nr_nodes;
	std::map<gchar *,std::list<dNode> > dup_list;

	fTree();
	~fTree();

	void insert_file(gchar *name)
	{
		struct stat statb;
		gsize size;

		lstat(name, &statb);
		size = statb.st_size;

		INC_NR_FILES();
		INC_BYTES_TOTAL(size);

		if (!this->root)
		{
			this->root = new fNode();
			this->root->name = strdup(name);
			this->root->size = size;
			this->root->left = NULL;
			this->root->right = NULL;
			this->root->parent = NULL;
			this->nr_nodes += 1;

			return;
		}

		fNode *n = this->root;

		while (true)
		{
			if (size < n->size)
			{
				if (!n->left)
				{
					n->left = new fNode();
					n->left->name = strdup(name);
					n->left->size = size;
					n->left->parent = n;
					this->nr_nodes += 1;
#ifdef DEBUG
					std::cerr << "Inserted @ " << &n->left << std::endl;
#endif
					return;
				}
				else
				{
					n = n->left;
					continue;
				}
			}
			else
			if (size > n->size)
			{
				if (!n->right)
				{
					n->right = new fNode();
					n->right->name = strdup(name);
					n->right->size = size;
					n->right->parent = n;
					this->nr_nodes += 1;
#ifdef DEBUG
					std::cerr << "Inserted @ %p " << &n->right << std::endl;
#endif
					return;
				}
				else
				{
					n = n->right;
					continue;
				}
			}
			else /* size == n->size */
			{
				if (n->has_digest() == false)
				{
/*
 * get_file_digest() returns pointer to static memory so we
 * must use strdup() to save it.
 */
					gchar *tmp = get_file_digest(n->name);
					if (!tmp)
						return;

					n->digest = strdup(tmp);
					n->got_digest(true);
#ifdef DEBUG
					std::cerr << "Got digest of \"" << n->name << "\" at current node: " << n->digest << std::endl;
#endif
				}

/*
 * Don't need to strdup() this one because either we
 * will find that we don't need to keep it (cause the
 * hash is already in our bucket of digests), or we will
 * be calling strdup() later to save it at the end of
 * the bucket.
 */
				gchar *cur_digest = get_file_digest(name);

				if (!cur_digest)
					return;

#ifdef DEBUG
				std::cerr << "Got digest of current file \"" << name << "\": " << cur_digest << std::endl;
#endif

				if (!memcmp(cur_digest, n->digest, DIGEST_SIZE))
				{
					INC_NR_FILES();
					INC_BYTES_WASTED(size);

					if (n->been_added() == false)
					{
#ifdef DEBUG
						std::cerr << "Adding duplicate file to linked list" << std::endl;
#endif
						this->add_dup_file(n->name, n->digest);
					}

#ifdef DEBUG
					std::cerr << "Adding duplicate file to linked list" << std::endl;
#endif
					this->add_dup_file(name, cur_digest);

					break;
				}
				else
				{
/*
 * O(N) check of our bucket of digests to see if this
 * hash is already in there. If not, save this one
 * at the end of the bucket.
 */
#ifdef DEBUG
					std::cerr << "Searching bucket for matching digest" << std::endl;
#endif
					if (n->nr_items_bucket() > 0)
					{
						gint i;
						gint nr_bucket = n->nr_items_bucket();
						bool is_dup = false;
						gint idx;

						if (n->bucket_contains(cur_digest, DIGEST_SIZE))
						{
							INC_NR_FILES();
							INC_BYTES_WASTED(size);
#ifdef DEBUG
							std::cerr << "Found match in bucket. Inserting duplicate file to linked list" << std::endl;
#endif
							this->add_dup_file(name, cur_digest);
							is_dup = true;
						}

						if (is_dup == false)
						{
#ifdef DEBUG
							std::cerr << "No match in bucket. Adding file to bucket" << std::endl;
#endif
							fNode _node;
							_node.name = strdup(name);
							_node.digest = strdup(cur_digest);

							n->add_to_bucket(_node);
							return;
						}
					}
					else /* n->nr_items_bucket() == 0 */
					{
#ifdef DEBUG
						std::cerr << "Adding first file to bucket" << std::endl;
#endif
						fNode _node;
						_node.name = strdup(name);
						_node.digest = strdup(cur_digest);

						n->add_to_bucket(_node);
						return;
					}
				} /* cur_digest != n->digest */
			} /* size == n->size */
		} /* while (true) */
	}

	void show_duplicates(void)
	{
#ifdef DEBUG
		std::cerr << "Showing list of duplicate files" << std::endl;
#endif
		for (std::map<gchar *,std::list<dNode> >::iterator map_it = this->dup_list.begin(); map_it != this->dup_list.end(); ++map_it)
		{
			std::cerr << "** [" << map_it->first << "] **\n" << std::endl;
			for (std::list<dNode>::iterator node_it = map_it->second.begin(); node_it != map_it->second.end(); ++node_it)
			{
				std::cerr << node_it->name << std::endl;
			}

			std::cerr << "\n\n\n";
		}

#ifdef DEBUG
		for (std::map<gchar *,std::list<dNode> >::iterator map_it = this->dup_list.begin(); map_it != this->dup_list.end(); ++map_it)
			std::cerr << map_it->first << std::endl;
#endif
	}

	private:

	void add_dup_file(gchar *name, gchar *digest)
	{
		dNode node;

		node.name = strdup(name);

		for (std::map<gchar *,std::list<dNode> >::iterator map_it = this->dup_list.begin(); map_it != this->dup_list.end(); ++map_it)
		{
			if (!memcmp(digest, map_it->first, DIGEST_SIZE))
			{
				map_it->second.push_back(node);
				return;
			}
		}

		std::list<dNode> new_list;

		new_list.push_back(node);
		this->dup_list[digest] = new_list;

		return;
	}

	fNode *root;
};

fTree::fTree(void)
{
	this->nr_nodes = 0;
	this->root = NULL;
}

fTree::~fTree(void)
{
	for (std::map<gchar *,std::list<dNode> >::iterator map_iter = this->dup_list.begin(); map_iter != this->dup_list.end(); ++map_iter)
	{
		for (std::list<dNode>::iterator list_iter = map_iter->second.begin(); list_iter != map_iter->second.end(); ++list_iter)
		{
			if (list_iter->name)
			{
				free(list_iter->name);
				list_iter->name = NULL;
			}
		}
	}
	this->dup_list.clear();

	if (this->root)
	{
		fNode *node = this->root;
		fNode *parent = NULL;

		while (true)
		{
			if (!node->left && !node->right)
			{
				parent = node->parent;

				if (parent)
				{
					if (parent->left == node)
						parent->left = NULL;
					else
						parent->right = NULL;
				}

				free(node);

				if (!parent)
					break;

				node = parent;

				continue;
			}

			if (node->left)
			{
				while (node->left)
				{
					node = node->left;
				}
			}

			if (node->right)
			{
				node = node->right;
			}
		}
	} /* if this->root */
}

static fTree *tree;

static void
init_openssl(void)
{
	OPENSSL_config(NULL);
	OpenSSL_add_all_digests();
}

#define READ_BLOCK 4096

#define __ALIGN_SIZE(s) (((s) + 0xf) & ~(0xf))
static char const hexchars[17] = "0123456789abcdef";

gchar *
get_file_digest(gchar *path)
{
	struct stat statb;
	unsigned char *buffer = NULL;
	gsize toread;
	ssize_t n;
	EVP_MD_CTX *ctx;
	static unsigned char __digest[__ALIGN_SIZE(EVP_MAX_MD_SIZE + 1)];
	static unsigned char __hex[__ALIGN_SIZE(EVP_MAX_MD_SIZE * 2 + 1)];
	guint dlen = 0;
	gint fd = -1;

	lstat(path, &statb);

	memset(__digest, 0, DIGEST_SIZE / 2);
	memset(__hex, 0, DIGEST_SIZE);

	if ((fd = open(path, O_RDONLY)) < 0)
	{
		switch(errno)
		{
			case EPERM:
				std::cerr << "No permission to open \"" << path << "\"" << std::endl;
				break;
			default:
				std::cerr << "Failed to open \"" << path << "\" " << strerror(errno) << std::endl;
		}

		return NULL;
	}

	buffer = (unsigned char *)calloc(__ALIGN_SIZE(statb.st_size+1), 1);

	toread = statb.st_size;

	if (!(ctx = EVP_MD_CTX_create()))
	{
		std::cerr << "get_file_digest: failed to create MD CTX" << std::endl;
		goto fail;
	}

	if (1 != EVP_DigestInit_ex(ctx, CTX.digest_func, NULL))
	{
		std::cerr << "get_file_digest: failed to initialise MD CTX" << std::endl;
		goto fail;
	}

	while (toread > 0 && (n = read(fd, buffer, READ_BLOCK)))
	{
		if (n < 0)
		{
			if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN)
				continue;
			else
			{
				std::cerr << "get_file_digest: failed to read from \"" << path << "\"" << std::endl;
				goto fail;
			}
		}

		buffer[n] = 0;
		if (1 != EVP_DigestUpdate(ctx, buffer, n))
		{
			std::cerr << "get_file_digest: failed to update hash digest" << std::endl;
			goto fail;
		}

		toread -= n;
	}

	if (1 != EVP_DigestFinal_ex(ctx, __digest, &dlen))
	{
		std::cerr << "get_file_digest: failed to finalise hash digest" << std::endl;
		goto fail;
	}

	EVP_MD_CTX_destroy(ctx);
	free(buffer);

	gint i;
	gint k;

/*
 * Hexlify the digest.
 */
	for (i = 0, k = 0; i < (gint)dlen; ++i)
	{
		__hex[k++] = hexchars[((__digest[i] >> 4) & 0xf)];
		__hex[k++] = hexchars[(__digest[i] & 0xf)];
	}

	__hex[k] = 0;

	close(fd);
	fd = -1;
	return (gchar *)__hex;

	fail:

	close(fd);
	fd = -1;
	free(buffer);
	EVP_MD_CTX_destroy(ctx);
	return NULL;
}

static gint
scan_files(gchar *dir)
{
	gsize len = strlen(dir);
	gchar *p = (dir + len);
	struct stat statb;
	DIR *dirp;
	struct dirent *dinf;

	if (*(p-1) != '/')
	{
		*p++ = '/';
		*p = 0;
		++len;
	}

	//std::cerr << "Scanning directory \"" << dir << "\"" << std::endl;
	dirp = fdopendir(open(dir, O_DIRECTORY));

	if (!dirp)
		return 0;

	assert(dirp);

	while ((dinf = readdir(dirp)))
	{
		if (!strcmp(".", dinf->d_name) ||
			!strcmp("..", dinf->d_name) ||
			dinf->d_name[0] == '.')
			continue;

		strcpy(p, dinf->d_name);
		lstat(dir, &statb);

		if (S_ISDIR(statb.st_mode))
		{
			scan_files(dir);
		}
		else
		if (S_ISREG(statb.st_mode))
		{
#ifdef DEBUG
			std::cerr << "Inserting \"" << dir << "\" into binary tree" << std::endl;
#endif
			tree->insert_file(dir);
		}
	}

	closedir(dirp);

	*p = 0;
	return 0;
}

static void
catch_sigint(int signo)
{
	if (signo != SIGINT)
		return;

	siglongjmp(__root_env__, 1);
}

static void
__attribute__((constructor)) __pollux_init(void)
{
	memset(&sigint_new, 0, sizeof(sigint_new));

	sigint_new.sa_handler = catch_sigint;
	sigint_new.sa_flags = 0;

	if (sigaction(SIGINT, &sigint_old, &sigint_new) < 0)
	{
		std::cerr << "__pollux_init: failed to set signal handler for SIGINT" << std::endl;
		goto fail;
	}

	return;

	fail:
	exit(EXIT_FAILURE);
}

void
on_option_select(GtkWidget *widget, gpointer data)
{
	if (CTX.scanning == TRUE)
		return;

	gint _opt = *(gint *)data;
	gboolean _active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

	switch(_opt)
	{
		case OPTION_NO_HIDDEN:
			if (_active == TRUE)
				CTX.runtime_options |= OPT_NO_HIDDEN;
			else
				CTX.runtime_options &= ~(OPT_NO_HIDDEN);
#ifdef DEBUG
				std::cerr << "option no hidden toggled" << std::endl;
#endif
			break;
		case OPTION_DELETE_ALL:
			if (_active == TRUE)
				CTX.runtime_options |= OPT_DELETE_ALL;
			else
				CTX.runtime_options &= ~(OPT_DELETE_ALL);
			break;
#ifdef DEBUG
				std::cerr << "option delete all toggled" << std::endl;
#endif
		default:
			break;
	}

	return;
}

void
on_digest_select(GtkWidget *widget, gpointer data)
{
	if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(widget)) == FALSE)
		return;

	struct Digest *dgst_ptr = NULL;
/*
 * Iterate over the linked list of struct Digest objects and
 * change those that != WIDGET to FALSE.
 */
	for (GList *list_iter = g_list_first(list_digests); list_iter; list_iter = list_iter->next)
	{
		dgst_ptr = (struct Digest *)list_iter->data;

		if (!dgst_ptr)
		{
			std::cerr << "*** list_iter->data is NULL ***" << std::endl;
			continue;
		}

		if (dgst_ptr->item == widget)
		{
			CTX.digest_type = dgst_ptr->type;

			switch(CTX.digest_type)
			{
				case DIGEST_SHA256:
					CTX.digest_func = (EVP_MD *)EVP_sha256();
					DIGEST_SIZE = (EVP_MD_size(EVP_sha256()) * 2);
					break;
				case DIGEST_SHA512:
					CTX.digest_func = (EVP_MD *)EVP_sha512();
					DIGEST_SIZE = (EVP_MD_size(EVP_sha512()) * 2);
					break;
				default:
					CTX.digest_func = (EVP_MD *)EVP_md5();
					DIGEST_SIZE = (EVP_MD_size(EVP_md5()) * 2);
			}

			continue;
		}

		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(dgst_ptr->item), FALSE);
	}
}

void
on_choose_directory(GtkWidget *widget, gpointer data)
{
	if (CTX.scanning == TRUE)
		return;

	gchar tmp[PATH_MAX];
	gchar *p;

	p = gtk_file_chooser_get_uri(GTK_FILE_CHOOSER(widget));
	p += strlen("file://");
	strcpy(CTX.start_at, p);

#ifdef DEBUG
	std::cout << "User chose " << gtk_file_chooser_get_uri(GTK_FILE_CHOOSER(widget)) << std::endl;
#endif

	return;
}

void
on_select_all_for_digest(GtkWidget *widget, gpointer data)
{
	gchar *_digest = (gchar *)data;
	gboolean _active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

	if (_active)
		std::cerr << "User selected all for digest " << _digest << std::endl;
	else
		std::cerr << "User de-selected all for digest " << _digest << std::endl;

	return;
}

void
on_select_file(GtkWidget *widget, gpointer data)
{
	gchar *_name = (gchar *)data;
	gboolean _active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

	if (_active)
		std::cerr << "User selected file " << _name << std::endl;
	else
		std::cerr << "User de-selected file " << _name << std::endl;

	return;
}

/**
 * This is the callback for a toggle button cell renderer.
 * The path to the node is in string format in PATH. We will
 * pass the tree store in DATA.
 */
void
on_row_toggled(GtkCellRendererToggle *cell, gchar *path, gpointer data)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gboolean valid;
	gboolean _active;
	GtkWidget *_check;

	std::cerr << "on_row_toggled() called" << std::endl;

	model = GTK_TREE_MODEL(data);
	valid = gtk_tree_model_get_iter_from_string(model, &iter, path);

	if (valid)
	{
		gboolean _all;
		gtk_tree_model_get(model, &iter, COL_SELECT, &_check, COL_IS_ALL, &_all, -1);

		_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_check));

		if (_all == TRUE)
		{
			GtkTreeIter child;
			GtkTreePath *_path;
			gchar *_digest;
			gchar *filepath;

			valid = TRUE;

			gtk_tree_model_get(model, &iter, COL_PATH, &_digest, -1);
			_path = gtk_tree_path_new_from_string(path);

			if (_active)
			{
				gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_check), FALSE);
				gtk_cell_renderer_toggle_set_active(GTK_CELL_RENDERER_TOGGLE(cell), FALSE);
				std::cerr << "User de-selected all files with digest \"" << _digest << "\":" << std::endl;
			}
			else
			{
				gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_check), TRUE);
				gtk_cell_renderer_toggle_set_active(GTK_CELL_RENDERER_TOGGLE(cell), TRUE);
				std::cerr << "User selected all files with digest \"" << _digest << "\":" << std::endl;
			}


			gtk_tree_path_down(_path);

			while (true)
			{
				path = gtk_tree_path_to_string(_path);
				valid = gtk_tree_model_get_iter_from_string(model, &iter, path);

				if (!valid)
					break;

				gtk_tree_model_get(model, &iter, COL_SELECT, &_check, COL_PATH, &filepath, -1);

				if (_active)
				{
					gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_check), FALSE);
					gtk_cell_renderer_toggle_set_active(GTK_CELL_RENDERER_TOGGLE(cell), FALSE);
				}
				else
				{
					gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_check), TRUE);
					gtk_cell_renderer_toggle_set_active(GTK_CELL_RENDERER_TOGGLE(cell), TRUE);
				}

				std::cerr << filepath << std::endl;
				gtk_tree_path_next(_path);
			}

			std::cerr << std::endl;
			/* Get the file paths from all the children of this node */
		}
		else
		{
			gchar *filepath;
			GtkWidget *_check_box;

			gtk_tree_model_get(model, &iter, COL_SELECT, &_check_box, COL_PATH, &filepath, -1);

			if (_active)
			{
				gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_check), FALSE);
				gtk_cell_renderer_toggle_set_active(GTK_CELL_RENDERER_TOGGLE(cell), FALSE);
				std::cerr << "User de-selected file \"" << filepath << "\"" << std::endl;
			}
			else
			{
				gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_check), TRUE);
				gtk_cell_renderer_toggle_set_active(GTK_CELL_RENDERER_TOGGLE(cell), TRUE);
				std::cerr << "User selected file \"" << filepath << "\"" << std::endl;
			}
		}
	}
}

void
on_start_scan(GtkWidget *widget, gpointer data)
{
	if (CTX.scanning == TRUE)
		return;

	gint retval;

	tree = new fTree();

	CTX.scanning = TRUE;
	retval = scan_files(CTX.start_at);

	if (retval == -1)
	{
		g_error("Scanning failed...");
		delete tree;
		return;
	}

	GtkTreeIter iter;
	GtkTreeIter child;

	static gchar __size[32];
	static gchar __created[128];
	static gchar __modified[128];
	struct tm tm;
	struct stat statb;

#define clear_struct(s) memset((s), 0, sizeof((*s)))

	clear_struct(&statb);

	store = gtk_tree_store_new(NR_COLUMNS, G_TYPE_POINTER, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
	g_assert(store);

	for (std::map<gchar *,std::list<dNode> >::iterator map_iter = tree->dup_list.begin(); map_iter != tree->dup_list.end(); ++map_iter)
	{
		check_button = gtk_check_button_new();
		g_assert(check_button);

		gtk_tree_store_append(store, &iter, NULL);
		gtk_tree_store_set(store, &iter,
				COL_SELECT, (gpointer)check_button,
				COL_IS_ALL, TRUE,
				COL_PATH, map_iter->first,
				COL_SIZE, " ",
				COL_TIME_CREATED, " ",
				COL_TIME_MODIFIED, " ",
				-1);

		for (std::list<dNode>::iterator list_iter = map_iter->second.begin(); list_iter != map_iter->second.end(); ++list_iter)
		{
			clear_struct(&statb);

			lstat(list_iter->name, &statb);

			sprintf(__size, "    %lu bytes    ", statb.st_size);

			clear_struct(&tm);

			if (likely(gmtime_r((time_t *)&statb.st_ctime, &tm)))
				strftime(__created, 128, "    %d %B %Y %H:%M:%S %Z    ", &tm);
			else
				strcpy(__created, "Unknown");

			clear_struct(&tm);

			if (likely(gmtime_r((time_t *)&statb.st_mtime, &tm)))
				strftime(__modified, 128, "  %d %B %Y %H:%M:%S %Z       ", &tm);
			else
				strcpy(__modified, "Unknown");

			check_button = gtk_check_button_new();
			g_assert(check_button);

			gtk_tree_store_append(store, &child, &iter);

			gtk_tree_store_set(store, &child,
					COL_SELECT, (gpointer)check_button,
					COL_IS_ALL, FALSE,
					COL_PATH, list_iter->name,
					COL_SIZE, __size,
					COL_TIME_CREATED, __created,
					COL_TIME_MODIFIED, __modified,
					-1);
		}
	}

	view = gtk_tree_view_new();
	renderer = gtk_cell_renderer_text_new();
	toggle_renderer = gtk_cell_renderer_toggle_new();

	g_signal_connect(toggle_renderer, "toggled", G_CALLBACK(on_row_toggled), (gpointer)store);

	gtk_tree_view_set_model(GTK_TREE_VIEW(view), GTK_TREE_MODEL(store));
	g_object_unref(G_OBJECT(store));

	//g_object_set(G_OBJECT(renderer), "cell-background", "LightGrey", "cell-background-set", TRUE, NULL);

/*
 * First column is the check button. Second column is
 * not visible in the view. It stores a boolean value
 * that the toggle button callback func will use to
 * know whether the row check corresponds to a single
 * row that is a child of the hash digest, or whether
 * it is the row with the digest, meaning we select
 * all the rows that are children of it.
 */
	gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(view),
			-1, result_columns[0].name, toggle_renderer, 0, NULL);

/*
 * Do not need to add second column to the view because we don't
 * want it to be visible!!
 */

	for (gint i = 2; i < NR_COLUMNS; ++i)
	{
		gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(view),
					-1, result_columns[i].name, renderer, "text", i, NULL);
	}

	results_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(results_window), "Duplicate Files");
	gtk_widget_set_size_request(results_window, SCROLLING_WINDOW_WIDTH, SCROLLING_WINDOW_HEIGHT);
	scrolling = gtk_scrolled_window_new(NULL, NULL);
	gtk_widget_set_size_request(scrolling, SCROLLING_WINDOW_WIDTH, SCROLLING_WINDOW_HEIGHT);
	gtk_container_add(GTK_CONTAINER(scrolling), view);
	gtk_container_add(GTK_CONTAINER(results_window), scrolling);

	delete tree;
	CTX.scanning = FALSE;

	gtk_widget_show_all(results_window);
}

void
create_menu_bar(void)
{
	menu_bar = gtk_menu_bar_new();
	item_file = gtk_menu_item_new_with_label("File");
	file_menu = gtk_menu_new();
	item_file_quit = gtk_menu_item_new_with_label("Quit");

	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item_file), file_menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), item_file_quit);

	options_menu = gtk_menu_new();
	item_options = gtk_menu_item_new_with_label("Options");
	digests_menu = gtk_menu_new();
	item_digests = gtk_menu_item_new_with_label("Digests");

	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item_options), options_menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(options_menu), item_digests);
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item_digests), digests_menu);

	list_digests = NULL;

	for (gint i = 0; i < NR_DIGESTS; ++i)
	{
		menu_digests[i].item = gtk_check_menu_item_new_with_label(menu_digests[i].name);

		if (menu_digests[i].type == __DEFAULT_DIGEST)
			gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_digests[i].item), TRUE);

		g_signal_connect(menu_digests[i].item, "toggled", G_CALLBACK(on_digest_select), NULL);
		gtk_menu_shell_append(GTK_MENU_SHELL(digests_menu), menu_digests[i].item);

		list_digests = g_list_append(list_digests, (gpointer)&menu_digests[i]);
	}

	gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), item_file);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), item_options);

	gtk_grid_attach(GTK_GRID(grid), menu_bar, 0, 0, 1, 1);

	return;
}

void
create_window(void)
{
	window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(window), PROG_NAME " v"POLLUX_BUILD);
	gtk_window_set_default_size(GTK_WINDOW(window), WIN_DEFAULT_WIDTH, WIN_DEFAULT_HEIGHT);
	gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

	grid = gtk_grid_new();
	gtk_grid_set_row_homogeneous(GTK_GRID(grid), FALSE);
	gtk_grid_set_row_spacing(GTK_GRID(grid), MAIN_GRID_ROW_SPACING);
	gtk_grid_set_column_homogeneous(GTK_GRID(grid), FALSE);
	gtk_grid_set_column_spacing(GTK_GRID(grid), MAIN_GRID_COL_SPACING);
	gtk_widget_set_size_request(grid, WIN_DEFAULT_WIDTH, WIN_DEFAULT_HEIGHT);

	create_menu_bar();
	
	gtk_container_add(GTK_CONTAINER(window), grid);

/*
 * Load and keep our application icon in a pixbuf.
 */
	GError *error = NULL;

	icon_pixbuf = gdk_pixbuf_new_from_file_at_size(
			POLLUX_LOGO,
			PROG_ICON_WIDTH,
			PROG_ICON_HEIGHT,
			&error);

	if (error)
	{
		g_error("Error loading application icon: (%s)\n", error->message);
		g_error_free(error);
	}

	icon_pixbuf_small = gdk_pixbuf_new_from_file_at_size(
			POLLUX_LOGO,
			PROG_ICON_SMALL_WIDTH,
			PROG_ICON_SMALL_HEIGHT,
			&error);

	if (error)
	{
		g_error("Error getting application icon pixbuf: (%s)\n", error->message);
		g_error_free(error);
	}

	gtk_window_set_icon(GTK_WINDOW(window), icon_pixbuf);
	g_object_unref(G_OBJECT(icon_pixbuf));


	static const gchar *authors[] = PROG_AUTHORS;
	static const gchar *title = "About " PROG_NAME;

	gtk_show_about_dialog(
			GTK_WINDOW(window),
			"program-name", PROG_NAME,
			"logo", icon_pixbuf,
			"title", title,
			"version", "Version "POLLUX_BUILD,
			"comments", PROG_COMMENTS,
			"authors", authors,
			"website", PROG_WEBSITE,
			"copyright", PROG_LICENCE,
			NULL);

	image = gtk_image_new_from_pixbuf(icon_pixbuf);
	image_small = gtk_image_new_from_pixbuf(icon_pixbuf_small);

	list_box = gtk_list_box_new();
	gtk_widget_set_size_request(list_box, 120, 100);

#if 0
	stats_nr_files = gtk_label_new("#Files");
	stats_nr_dups = gtk_label_new("#Dups");
	stats_nr_bytes_total = gtk_label_new("#Memory");
	stats_nr_bytes_wasted = gtk_label_new("#Wasted");

	sep1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	sep2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	sep3 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	sep4 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);

	gtk_list_box_prepend(GTK_LIST_BOX(list_box), stats_nr_files);
	gtk_list_box_prepend(GTK_LIST_BOX(list_box), sep1);
	gtk_list_box_prepend(GTK_LIST_BOX(list_box), stats_nr_dups);
	gtk_list_box_prepend(GTK_LIST_BOX(list_box), sep2);
	gtk_list_box_prepend(GTK_LIST_BOX(list_box), stats_nr_bytes_total);
	gtk_list_box_prepend(GTK_LIST_BOX(list_box), sep3);
	gtk_list_box_prepend(GTK_LIST_BOX(list_box), stats_nr_bytes_wasted);
	gtk_list_box_prepend(GTK_LIST_BOX(list_box), sep4);
#endif
/*
 * Create button to start scan that has a small
 * version of application logo contained within.
 */
	button_start_scan = gtk_button_new();
	gtk_container_add(GTK_CONTAINER(button_start_scan), image_small);
	g_signal_connect(button_start_scan, "clicked", G_CALLBACK(on_start_scan), NULL);

/*
 * Create a button for choosing the directory
 * to start the scan in (on clicking, a menu
 * appears with a list of directories to choose
 * from.
 */
	label_choose_dir = gtk_label_new("Choose directory...");
	button_choose_dir = gtk_file_chooser_button_new("Select starting directory", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(button_choose_dir), getenv("HOME"));
	g_signal_connect(button_choose_dir, "file-set", G_CALLBACK(on_choose_directory), NULL);

/*
 * Create a box of check buttons for runtime options
 * which will be placed just to the right of the
 * vertical separator.
 */
	options_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_box_set_homogeneous(GTK_BOX(options_box), FALSE);
	gtk_box_set_spacing(GTK_BOX(options_box), 1);

	for (gint i = 0; i < NR_OPTIONS; ++i)
	{
		options[i].widget = gtk_check_button_new_with_label(options[i].name);
		g_signal_connect(options[i].widget, "toggled", G_CALLBACK(options[i].func), &options[i].option);
		gtk_box_pack_start(GTK_BOX(options_box), options[i].widget, FALSE, FALSE, 10);
	}

	status_bar = gtk_statusbar_new();

/* gtk_grid_attach(GtkGrid *grid, GtkWidget *widget, gint left, gint top, gint width, gint height); */
	gtk_grid_attach(GTK_GRID(grid), image, 2, 2, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), list_box, 0, 2, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), options_box, 2, 3, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), label_choose_dir, 2, 4, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), button_choose_dir, 2, 5, 1, 1);
	gtk_grid_attach_next_to(GTK_GRID(grid), button_start_scan, button_choose_dir, GTK_POS_RIGHT, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), status_bar, 0, 6, 1, 1);

	gtk_widget_show_all(window);

	return;
}

int
main(int argc, char *argv[])
{
	struct stat statb;
	gint status;

	init_openssl();

	CTX.digest_func = (EVP_MD *)EVP_md5();
	DIGEST_SIZE = (EVP_MD_size(EVP_md5()) * 2);

	app = gtk_application_new(PROG_NAME_DBUS, G_APPLICATION_FLAGS_NONE);
	g_signal_connect(app, "activate", G_CALLBACK(create_window), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(G_OBJECT(app));

	g_list_free(list_digests);
#if 0
	tree = new fTree();

	std::cerr << "Pollux build " << POLLUX_BUILD << " (written in C++)" << std::endl;

	if (argc < 2)
	{
		std::cerr << PROG_NAME << " <directory>" << std::endl;
		exit(EXIT_FAILURE);
	}


	lstat(argv[1], &statb);

	if (!S_ISDIR(statb.st_mode))
	{
		std::cerr << "\"" << argv[1] << "\" is not a directory!" << std::endl;
		goto fail;
	}

	if (sigsetjmp(__root_env__, 1) != 0)
	{
		std::cerr << "Caught user-generated signal" << std::endl;
		goto out_release_mem;
	}

	init_openssl();

	std::cerr << "Starting scan in directory " << argv[1] << std::endl;

	if (scan_files(argv[1]) == -1)
		goto fail;

	tree->show_duplicates();

	out_release_mem:
#endif

	return 0;
}
