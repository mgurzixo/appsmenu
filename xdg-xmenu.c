/* Author: Lu Xu <oliver_lew at outlook dot com>
 * License: MIT
 * References: https://specifications.freedesktop.org/desktop-entry-spec
 *             https://specifications.freedesktop.org/icon-theme-spec
 */

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ini.h>

/* for long texts */
#define LLEN 1024
/* for file paths */
#define MLEN 256
/* for simple names or directories */
#define SLEN 128

#define LEN(X) (sizeof(X) / sizeof(X[0]))

#define LIST_FREE(L, TYPE) \
	for (TYPE *p = (L)->next, *tmp; p; tmp = p->next, free(p), p = tmp) ; \
	(L)->next = NULL;

#define LIST_INSERT(L, TEXT, N) { \
	List *tmp = malloc(sizeof(List)); \
	strncpy(tmp->text, TEXT, N); \
	tmp->next = (L)->next; \
	(L)->next = tmp; \
}

struct Option {
	char *fallback_icon;
	char *icon_theme;
	char *terminal;
	char *xmenu_cmd;
	int dry_run;
	int dump;
	int icon_size;
	int no_genname;
	int no_icon;
	int scale;
} option = {
	.fallback_icon = "application-x-executable",
	.icon_size = 24,
	.scale = 1,
	.terminal = "xterm",
	.xmenu_cmd = "xmenu"
};

typedef struct App {
	/* from desktop entry file */
	char category[SLEN];
	char exec[MLEN];
	char genericname[SLEN];
	char icon[SLEN];
	char name[SLEN];
	char path[MLEN];
	int terminal;
	/* derived attributes */
	char entry_path[MLEN];
	char xmenu_entry[LLEN];
	int not_show;
	struct App *next;
} App;

typedef struct List {
	char text[SLEN];
	struct List *next;
} List;

struct Category2Name {
	char *category;
	char *name;
} xdg_categories[] = {
	{"Audio", "Multimedia"},
	{"AudioVideo", "Multimedia"},
	{"Development", "Development"},
	{"Education", "Education"},
	{"Game", "Games"},
	{"Graphics", "Graphics"},
	{"Network", "Internet"},
	{"Office", "Office"},
	{"Others", "Others"},
	{"Science", "Science"},
	{"Settings", "Settings"},
	{"System", "System"},
	{"Utility", "Accessories"},
	{"Video", "Multimedia"}
};

struct Name2Icon {
	char *category;
	char *icon;
} category_icons[] = {
	{"Accessories", "applications-accessories"},
	{"Development", "applications-development"},
	{"Education", "applications-education"},
	{"Games", "applications-games"},
	{"Graphics", "applications-graphics"},
	{"Internet", "applications-internet"},
	{"Multimedia", "applications-multimedia"},
	{"Office", "applications-office"},
	{"Others", "applications-other"},
	{"Science", "applications-science"},
	{"Settings", "preferences-desktop"},
	{"System", "applications-system"}
};

const char *usage_str =
	"xdg-xmenu [-deGhIn] [-b ICON] [-i THEME] [-s SIZE] [-S SCALE] [-t TERMINAL] [-x CMD] [-- ...]\n\n"
	"Generate XDG menu for xmenu.\n\n"
	"Options:\n"
	"  -h          Show this help message and exit\n"
	"  -b ICON     Fallback icon name, default is application-x-executable\n"
	"  -d          Dump generated menu, do not run xmenu\n"
	"  -G          Do not show generic name of the app\n"
	"  -i THEME    Icon theme for app icons. Default to gtk3 settings\n"
	"  -I          Disable icon in xmenu\n"
	"  -n          Do not run app, output to stdout\n"
	"  -s SIZE     Icon theme for app icons\n"
	"  -S SCALE    Icon size scale factor, work with HiDPI screens\n"
	"  -t TERMINAL Terminal emulator to use, default is xterm\n"
	"  -x CMD      Xmenu command to use, default is xmenu\n"
	"Note:\n\tOptions after `--' are passed to xmenu\n";


char PATH[LLEN];
char HOME[SLEN];
char XDG_DATA_HOME[SLEN];
char XDG_DATA_DIRS[LLEN];
char XDG_CONFIG_HOME[SLEN];
char XDG_CURRENT_DESKTOP[SLEN];
char DATA_DIRS[LLEN + MLEN];
char FALLBACK_ICON_PATH[MLEN];
char FALLBACK_ICON_THEME[SLEN] = "hicolor";
List icon_dirs, path_list, data_dirs_list, current_desktop_list;
App all_apps;

int check_desktop(const char *desktop_list);
int check_exec(const char *cmd);
void clean_up_lists();
void extract_main_category(char *category, const char *categories);
void find_all_apps();
void find_icon(char *icon_path, char *icon_name);
void find_icon_dirs();
void gen_entry(App *app);
void getenv_fb(char *dest, char *name, char *fallback, int n);
int handler_icon_dirs_theme(void *user, const char *section, const char *name, const char *value);
int handler_parse_app(void *user, const char *section, const char *name, const char *value);
int handler_set_icon_theme(void *user, const char *section, const char *name, const char *value);
void prepare_envvars();
void print_menu(FILE *fp);
void run_xmenu(int argc, char *argv[]);
void set_icon_theme();
int  spawn(const char *cmd, char *const argv[], int *fd_input, int *fd_output);
void split_to_list(List *list, const char *env_string, char *sep);

int check_desktop(const char *desktop_list) {
	for (List *desktop = current_desktop_list.next; desktop; desktop = desktop->next)
		if (strstr(desktop_list, desktop->text))
			return 1;
	return 0;
}

int check_exec(const char *cmd)
{
	char file[MLEN];
	struct stat sb;

	/* if command start with '/', check it directly */
	if (cmd[0] == '/')
		return stat(cmd, &sb) == 0 && sb.st_mode & S_IXUSR;

	for (List *dir = path_list.next; dir; dir = dir->next) {
		sprintf(file, "%s/%s", dir->text, cmd);
		if (stat(file, &sb) == 0 && sb.st_mode & S_IXUSR)
			return 1;
	}
	return 0;
}

void clean_up_lists()
{
	LIST_FREE(&icon_dirs, List);
	LIST_FREE(&path_list, List);
	LIST_FREE(&data_dirs_list, List);
	LIST_FREE(&current_desktop_list, List);
}

void extract_main_category(char *category, const char *categories)
{
	List list_categories = {0}, *s;

	split_to_list(&list_categories, categories, ";");
	for (s = list_categories.next; s; s = s->next)
		for (int i = 0; i < LEN(xdg_categories); i++)
			if (strcmp(xdg_categories[i].category, s->text) == 0)
				strcpy(category, xdg_categories[i].name);

	LIST_FREE(&list_categories, List);
}

void find_icon(char *icon_path, char *icon_name)
{
	char test_path[MLEN];
	static const char *exts[] = {"svg", "png", "xpm"};

	for (List *dir = icon_dirs.next; dir; dir = dir->next) {
		for (int i = 0; i < 3; i++) {
			snprintf(test_path, MLEN, "%s/%s.%s", dir->text, icon_name, exts[i]);
			if (access(test_path, F_OK) == 0) {
				strncpy(icon_path, test_path, MLEN);
				return;
			}
		}
	}
	strncpy(icon_path, FALLBACK_ICON_PATH, MLEN);
}

void find_icon_dirs()
{
	int res, len_parent;
	char dir_parent[SLEN] = {0}, index_theme[MLEN] = {0};

	for (List *dir = data_dirs_list.next; dir; dir = dir->next) {
		snprintf(index_theme, MLEN, "%s/icons/%s/index.theme", dir->text, option.icon_theme);
		if (access(index_theme, F_OK) == 0) {
			if ((res = ini_parse(index_theme, handler_icon_dirs_theme, &icon_dirs)) < 0)
				fprintf(stderr, "Desktop file parse failed: %d\n", res);
			/* mannually call. a hack to process the end of file */
			handler_icon_dirs_theme(&icon_dirs, "", NULL, NULL);
		}

		/* prepend dirs with parent path */
		len_parent = snprintf(dir_parent, SLEN, "%s/icons/%s/", dir->text, option.icon_theme);
		for (List *idir = icon_dirs.next; idir; idir = idir->next) {
			/* FIXME: This is hacky, change this */
			if (idir->text[0] != '/') {
				strncpy(idir->text + len_parent, idir->text, strlen(idir->text));
				memcpy(idir->text, dir_parent, strlen(dir_parent));
			}
		}
	}

	LIST_INSERT(&icon_dirs, "/usr/share/pixmaps", SLEN);
}

void gen_entry(App *app)
{
	char *perc, field, replace_str[MLEN];
	char icon_path[MLEN], name[MLEN + 4], command[MLEN + SLEN], buffer[MLEN];

	if (app->terminal)
		sprintf(command, "%s -e %s", option.terminal, app->exec);
	else
		strcpy(command, app->exec);

	/* replace field codes */
	/* search starting from right, this way the starting position stays the same */
	while ((perc = strrchr(command, '%')) != NULL) {
		field = *(perc + 1);
		if (isalpha(field)) {
			memset(replace_str, 0, MLEN);
			if (field == 'c')
				strncpy(replace_str, app->entry_path, MLEN);
			else if (field == 'i' && strlen(app->icon) != 0)
				snprintf(replace_str, MLEN, "--icon %s", app->icon);
			else if (field == 'k')
				strncpy(replace_str, app->name, MLEN);
			strncpy(buffer, perc + 2, MLEN);
			snprintf(perc, MLEN - (perc - command), "%s%s", replace_str, buffer);
		}
	}

	if (!option.no_genname && strlen(app->genericname) > 0)
		sprintf(name, "%s (%s)", app->name, app->genericname);
	else
		strcpy(name, app->name);

	if (option.no_icon) {
		sprintf(app->xmenu_entry, "\t%s\t%s", name, command);
	} else {
		find_icon(icon_path, app->icon);
		sprintf(app->xmenu_entry, "\tIMG:%s\t%s\t%s", icon_path, name, command);
	}
}

/* getenv with fallback value */
void getenv_fb(char *dest, char *name, char *fallback, int n)
{
	char *env;

	if ((env = getenv(name))) {
		strncpy(dest, env, n);
	} else if (fallback) {
		if (fallback[0] != '/')  /* relative path to $HOME */
			snprintf(dest, n, "%s/%s", HOME, fallback);
		else
			strncpy(dest, fallback, n);
	}
}

/*
 * handler for ini_parse
 * match subdirectories in an icon theme folder by parsing an index.theme file
 * - the icon size is options.icon_size
 * - the icon theme will be specified as the parsed the index.theme file
 */
int handler_icon_dirs_theme(void *user, const char *section, const char *name, const char *value)
{
	/* static variables to preserve between function calls */
	static char subdir[32], type[16];
	static int size, minsize, maxsize, threshold, scale;

	List *dirs = (List *)user;
	while (dirs->next != NULL)
		dirs = dirs->next;

	if (strcmp(section, subdir) != 0 || (!name && !value)) {
		/* Check the icon size after finished parsing a section */
		if (scale == option.scale
			&& (((strcmp(type, "Threshold") == 0 || strlen(type) == 0)
					&& abs(size - option.icon_size) <= threshold)
				|| (strcmp(type, "Fixed") == 0
					&& size == option.icon_size)
				|| (strcmp(type, "Scalable") == 0
					&& minsize <= option.icon_size
					&& maxsize >= option.icon_size)))
			/* save dirs into this linked list */
			LIST_INSERT(dirs, subdir, SLEN);

		/* reset the current section */
		strncpy(subdir, section, 32);
		size = minsize = maxsize = -1;
		threshold = 2;  /* threshold fallback value */
		scale = 1;
		memset(type, 0, 16);
	}

	/* save the values into those static variables */
	if (!name && !value) { /* end of the file/section */
		return 1;
	} else if (strcmp(name, "Size") == 0) {
		size = atoi(value);
		if (minsize == -1)  /* minsize fallback value */
			minsize = size;
		if (maxsize == -1)  /* maxsize fallback value */
			maxsize = size;
	} else if (strcmp(name, "MinSize") == 0) {
		minsize = atoi(value);
	} else if (strcmp(name, "MaxSize") == 0) {
		maxsize = atoi(value);
	} else if (strcmp(name, "Threshold") == 0) {
		threshold = atoi(value);
	} else if (strcmp(name, "Scale") == 0) {
		scale = atoi(value);
	} else if (strcmp(name, "Type") == 0) {
		strncpy(type, value, 16);
	}

	return 1;
}

/* Handler for ini_parse, parse app info and save in App variable pointed by *user */
int handler_parse_app(void *user, const char *section, const char *name, const char *value)
{
	App *app = (App *)user;
	if (strcmp(section, "Desktop Entry") == 0) {
		if (strcmp(name, "Exec") == 0)
			strcpy(app->exec, value);
		else if (strcmp(name, "Icon") == 0)
			strcpy(app->icon, value);
		else if (strcmp(name, "Name") == 0)
			strcpy(app->name, value);
		else if (strcmp(name, "Terminal") == 0)
			app->terminal = strcmp(value, "true") == 0;
		else if (strcmp(name, "GenericName") == 0)
			strcpy(app->genericname, value);
		else if (strcmp(name, "Categories") == 0)
			extract_main_category(app->category, value);
		else if (strcmp(name, "Path") == 0)
			strcpy(app->path, value);

		if ((strcmp(name, "NoDisplay") == 0 && strcmp(value, "true") == 0)
			|| (strcmp(name, "Hidden") == 0 && strcmp(value, "true") == 0)
			|| (strcmp(name, "Type") == 0 && strcmp(value, "Application") != 0)
			|| (strcmp(name, "TryExec") == 0 && check_exec(value) == 0)
			|| (strcmp(name, "NotShowIn") == 0 && check_desktop(value))
			|| (strcmp(name, "OnlyShowIn") == 0 && !check_desktop(value)))
			app->not_show = 1;
	}
	return 1;
}

int handler_set_icon_theme(void *user, const char *section, const char *name, const char *value)
{
	if (strcmp(section, "Settings") == 0 && strcmp(name, "gtk-icon-theme-name") == 0)
		strcpy(FALLBACK_ICON_THEME, value);
	return 1;
}

void prepare_envvars()
{
	getenv_fb(PATH, "PATH", NULL, LLEN);
	getenv_fb(HOME, "HOME", NULL, SLEN);
	getenv_fb(XDG_DATA_HOME, "XDG_DATA_HOME", ".local/share", SLEN);
	getenv_fb(XDG_DATA_DIRS, "XDG_DATA_DIRS", "/usr/share:/usr/local/share", LLEN);
	getenv_fb(XDG_CONFIG_HOME, "XDG_CONFIG_HOME", ".config", SLEN);
	getenv_fb(XDG_CURRENT_DESKTOP, "XDG_CURRENT_DESKTOP", NULL, SLEN);
	snprintf(DATA_DIRS, LLEN + MLEN, "%s:%s", XDG_DATA_DIRS, XDG_DATA_HOME);

	/* NOTE: the string in the second argument will be modified, do not use again */
	split_to_list(&path_list, PATH, ":");
	split_to_list(&data_dirs_list, DATA_DIRS, ":");
	split_to_list(&current_desktop_list, XDG_CURRENT_DESKTOP, ":");
}

void set_icon_theme()
{
	int res;
	char gtk3_settings[MLEN] = {0}, *real_path;

	if (option.icon_theme && strlen(option.icon_theme) > 0)
		return;
	option.icon_theme = FALLBACK_ICON_THEME;

	/* Check gtk3 settings.ini file and overwrite default icon theme */
	snprintf(gtk3_settings, MLEN, "%s/gtk-3.0/settings.ini", XDG_CONFIG_HOME);
	if (access(gtk3_settings, F_OK) == 0) {
		real_path = realpath(gtk3_settings, NULL);
		if ((res = ini_parse(real_path, handler_set_icon_theme, NULL)) < 0)
			fprintf(stderr, "failed parse gtk settings\n");
		free(real_path);
	}
}

void find_all_apps()
{
	int res;
	char folder[MLEN] = {0}, path[LLEN] = {0};
	DIR *dir;
	struct dirent *entry;
	App *app;

	/* output all app in folder */
	for (List *data_dir = data_dirs_list.next; data_dir; data_dir = data_dir->next) {
		sprintf(folder, "%s/applications", data_dir->text);
		if ((dir = opendir(folder)) == NULL)
			continue;

		while ((entry = readdir(dir)) != NULL) {
			if (strcmp(strrchr(entry->d_name, '.'), ".desktop") != 0)
				continue;

			app = calloc(1, sizeof(App));
			sprintf(path, "%s/%s", folder, entry->d_name);
			strcpy(app->entry_path, path);
			if ((res = ini_parse(path, handler_parse_app, app)) < 0)
				fprintf(stderr, "Desktop file parse failed: %d\n", res);

			if (!app->not_show) {
				app->next = all_apps.next;
				all_apps.next = app;
				gen_entry(app);
			}
		}
		closedir(dir);
	}
}

void print_menu(FILE *fp)
{
	for (App* app = all_apps.next; app; app = app->next)
		fprintf(fp, "%s\n", app->xmenu_entry);
}

/*
 * User input 1--------->0 cmd 1-------->0 Output
 *             pfd_write        pdf_read
 * Create 2 pipes connecting cmd process with pfd_read[1] and pfd_write[0], and
 * return pfd_read[0] and pfd_write[1] back to user for read and write.
 */
int spawn(const char *cmd, char *const argv[], int *fd_input, int *fd_output)
{
	pid_t pid;
	int pfd_read[2], pfd_write[2];

	pipe(pfd_read);
	pipe(pfd_write);

	if ((pid = fork()) == 0) { /* in child */
		dup2(pfd_read[1], 1);
		dup2(pfd_write[0], 0);
		close(pfd_read[0]);
		close(pfd_write[1]);
		execvp(cmd, argv);
	} else if (pid > 0) { /* in parent */
		*fd_output = pfd_read[0];
		*fd_input = pfd_write[1];
	}

	close(pfd_read[1]);
	close(pfd_write[0]);
	return pid;
}

void split_to_list(List *list, const char *env_string, char *sep)
{
	char *buffer = strdup(env_string);
	List *tmp;

	for (char *p = strtok(buffer, sep); p; p = strtok(NULL, sep)) {
		tmp = malloc(sizeof(List));
		strncpy(tmp->text, p, SLEN);
		tmp->next = list->next;
		list->next = tmp;
	}

	free(buffer);
}

void run_xmenu(int argc, char *argv[])
{
	int pid, fd_input, fd_output;
	char **xmenu_argv, line[LLEN] = {0};
	FILE *fp;

	/* construct xmenu args for exec(3).
	 * +2 is for leading 'xmenu' and the ending NULL
	 * if no_icon is set, add another '-i' option */
	xmenu_argv = calloc(argc + option.no_icon ? 3 : 2, sizeof(char*));
	xmenu_argv[0] = option.xmenu_cmd;
	for (int i = 0; i < argc; i++)
		xmenu_argv[i + 1] = argv[i];
	if (option.no_icon && strcmp(option.xmenu_cmd, "xmenu") == 0)
		xmenu_argv[argc + 1] = "-i";

	pid = spawn(option.xmenu_cmd, xmenu_argv, &fd_input, &fd_output);
	fp = fdopen(fd_input, "w");
	print_menu(fp);
	fclose(fp);

	waitpid(pid, NULL, 0);
	if (read(fd_output, line, LLEN) > 0) {
		*strchr(line, '\n') = 0;
		if (option.dry_run)
			puts(line);
		else
			system(strcat(line, " &"));
	}
	close(fd_output);
}

int main(int argc, char *argv[])
{
	int opt;

	while ((opt = getopt(argc, argv, "b:dGhi:Ins:S:t:x:")) != -1) {
		switch (opt) {
			case 'b': option.fallback_icon = optarg; break;
			case 'd': option.dump = 1; break;
			case 'G': option.no_genname = 1; break;
			case 'i': option.icon_theme = optarg; break;
			case 'I': option.no_icon = 1; break;
			case 'n': option.dry_run = 1; break;
			case 's': option.icon_size = atoi(optarg); break;
			case 'S': option.scale = atoi(optarg); break;
			case 't': option.terminal = optarg; break;
			case 'x': option.xmenu_cmd = optarg; break;
			case 'h': default: puts(usage_str); exit(0); break;
		}
	}

	prepare_envvars();
	set_icon_theme();
	if (!option.no_icon) {
		find_icon_dirs();
		find_icon(FALLBACK_ICON_PATH, option.fallback_icon);
	}
	find_all_apps();

	if (option.dump)
		print_menu(stdout);
	else
		run_xmenu(argc - optind, argv + optind);

	clean_up_lists();
	return 0;
}
