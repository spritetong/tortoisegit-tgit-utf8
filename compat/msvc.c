/* For UTF-8, added by Sprite Tong, 12/1/2011. */
#define __XUTF8_INIT__

#include "../git-compat-util.h"
#include "win32.h"
#include <conio.h>
#include "../strbuf.h"

#include "mingw.c"

void show_chm_page(const char *git_cmd)
{
	//const char *page = cmd_to_page(git_cmd);
	struct strbuf page_path; /* it leaks but we exec bellow */

	struct stat st;
	const char *git_path = system_path(GIT_EXEC_PATH);
	int i;

	/* Check that we have a git documentation directory. */
	if (stat(mkpath("%s/TortoiseGit_en.chm", git_path), &st)
	    || !S_ISREG(st.st_mode))
		die("'%s': not a documentation directory.", git_path);

	strbuf_init(&page_path, 0);
	strbuf_addf(&page_path, "%s/TortoiseGit_en.chm::/git-%s(1).html", git_path, git_cmd);
	
	ShellExecute(NULL, "open","hh.exe", page_path.buf,NULL, SW_SHOW);
}
