/* uinfo.cc: user info (uid, gid, etc...)

   Copyright 1996, 1997, 1998, 1999, 2000, 2001, 2002, 2003 Red Hat, Inc.

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <pwd.h>
#include <unistd.h>
#include <winnls.h>
#include <wininet.h>
#include <utmp.h>
#include <limits.h>
#include <stdlib.h>
#include <lm.h>
#include <sys/cygwin.h>
#include "pinfo.h"
#include "security.h"
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "cygerrno.h"
#include "cygheap.h"
#include "registry.h"
#include "child_info.h"
#include "environ.h"
#include "pwdgrp.h"

/* Initialize the part of cygheap_user that does not depend on files.
   The information is used in shared.cc for the user shared.
   Final initialization occurs in uinfo_init */
void
cygheap_user::init ()
{
  char user_name[UNLEN + 1];
  DWORD user_name_len = UNLEN + 1;

  set_name (GetUserName (user_name, &user_name_len) ? user_name : "unknown");

  if (!wincap.has_security ())
    return;

  HANDLE ptok;
  DWORD siz;
  char pdacl_buf [sizeof (PTOKEN_DEFAULT_DACL) + ACL_DEFAULT_SIZE];
  PTOKEN_DEFAULT_DACL pdacl = (PTOKEN_DEFAULT_DACL) pdacl_buf;

  if (!OpenProcessToken (hMainProc, TOKEN_ADJUST_DEFAULT | TOKEN_QUERY,
			 &ptok))
    {
      system_printf ("OpenProcessToken(): %E");
      return;
    }
  if (!GetTokenInformation (ptok, TokenPrimaryGroup,
			    &groups.pgsid, sizeof (cygsid), &siz))
    system_printf ("GetTokenInformation (TokenPrimaryGroup): %E");

  /* Get the SID from current process and store it in effec_cygsid */
  if (!GetTokenInformation (ptok, TokenUser, &effec_cygsid, sizeof (cygsid), &siz))
    {
      system_printf ("GetTokenInformation (TokenUser): %E");
      goto out;
    }

  /* Set token owner to the same value as token user */
  if (!SetTokenInformation (ptok, TokenOwner, &effec_cygsid, sizeof (cygsid)))
    debug_printf ("SetTokenInformation(TokenOwner): %E");

  /* Add the user in the default DACL if needed */
  if (!GetTokenInformation (ptok, TokenDefaultDacl, pdacl, sizeof (pdacl_buf), &siz))
    system_printf ("GetTokenInformation (TokenDefaultDacl): %E");
  else if (pdacl->DefaultDacl) /* Running with security */
    {
      PACL pAcl = pdacl->DefaultDacl;
      PACCESS_ALLOWED_ACE pAce;

      for (int i = 0; i < pAcl->AceCount; i++)
        {
	  if (!GetAce (pAcl, i, (LPVOID *) &pAce))
	    system_printf ("GetAce: %E");
	  else if (pAce->Header.AceType == ACCESS_ALLOWED_ACE_TYPE
		   && effec_cygsid == &pAce->SidStart)
	    goto out;
	}
      pAcl->AclSize = &pdacl_buf[sizeof (pdacl_buf)] - (char *) pAcl;
      if (!AddAccessAllowedAce (pAcl, ACL_REVISION, GENERIC_ALL, effec_cygsid))
	system_printf ("AddAccessAllowedAce: %E");
      else if (FindFirstFreeAce (pAcl, (LPVOID *) &pAce), !(pAce))
	debug_printf ("FindFirstFreeAce %E");
      else
        {
          pAcl->AclSize = (char *) pAce - (char *) pAcl;
	  if (!SetTokenInformation (ptok, TokenDefaultDacl, pdacl, sizeof (* pdacl)))
	    system_printf ("SetTokenInformation (TokenDefaultDacl): %E");
        }
    }
 out:
  CloseHandle (ptok);
}

void
internal_getlogin (cygheap_user &user)
{
  struct passwd *pw = NULL;

  if (wincap.has_security ())
    {
      cygpsid psid = user.sid ();
      pw = internal_getpwsid (psid);
    }

  if (!pw && !(pw = internal_getpwnam (user.name ()))
      && !(pw = internal_getpwuid (DEFAULT_UID)))
    debug_printf ("user not found in augmented /etc/passwd");
  else
    {
      myself->uid = pw->pw_uid;
      myself->gid = pw->pw_gid;
      user.set_name (pw->pw_name);
      if (wincap.has_security ())
	{
	  cygsid gsid;
	  if (gsid.getfromgr (internal_getgrgid (pw->pw_gid)))
	    {
	      HANDLE ptok;
	      if (gsid != user.groups.pgsid
		  && OpenProcessToken (hMainProc, TOKEN_ADJUST_DEFAULT, &ptok))
	        {
		  /* Set primary group to the group in /etc/passwd. */
		  if (!SetTokenInformation (ptok, TokenPrimaryGroup,
					    &gsid, sizeof gsid))
		    debug_printf ("SetTokenInformation(TokenPrimaryGroup): %E");
		  else
		    user.groups.pgsid = gsid;
		  CloseHandle (ptok);
		}
	    }
	  else
	    debug_printf ("gsid not found in augmented /etc/group");
	}
    }
  (void) cygheap->user.ontherange (CH_HOME, pw);

  return;
}

void
uinfo_init ()
{
  if (child_proc_info && !cygheap->user.has_impersonation_tokens ())
    return;

  if (!child_proc_info)
    internal_getlogin (cygheap->user); /* Set the cygheap->user. */
  /* Conditions must match those in spawn to allow starting child
     processes with ruid != euid and rgid != egid. */
  else if (cygheap->user.issetuid ()
  	   && cygheap->user.saved_uid == cygheap->user.real_uid
	   && cygheap->user.saved_gid == cygheap->user.real_gid
	   && !cygheap->user.groups.issetgroups ())
    {
      cygheap->user.reimpersonate ();
      return;
    }
  else
    cygheap->user.close_impersonation_tokens ();

  cygheap->user.saved_uid = cygheap->user.real_uid = myself->uid;
  cygheap->user.saved_gid = cygheap->user.real_gid = myself->gid;
  cygheap->user.external_token = INVALID_HANDLE_VALUE;
  cygheap->user.internal_token = INVALID_HANDLE_VALUE;
  cygheap->user.current_token = INVALID_HANDLE_VALUE;
  cygheap->user.set_saved_sid ();	/* Update the original sid */
}

extern "C" char *
getlogin (void)
{
  return strcpy (_my_tls.locals.username, cygheap->user.name ());
}

extern "C" __uid32_t
getuid32 (void)
{
  return cygheap->user.real_uid;
}

extern "C" __uid16_t
getuid (void)
{
  return cygheap->user.real_uid;
}

extern "C" __gid32_t
getgid32 (void)
{
  return cygheap->user.real_gid;
}

extern "C" __gid16_t
getgid (void)
{
  return cygheap->user.real_gid;
}

extern "C" __uid32_t
geteuid32 (void)
{
  return myself->uid;
}

extern "C" __uid16_t
geteuid (void)
{
  return myself->uid;
}

extern "C" __gid32_t
getegid32 (void)
{
  return myself->gid;
}

extern "C" __gid16_t
getegid (void)
{
  return myself->gid;
}

/* Not quite right - cuserid can change, getlogin can't */
extern "C" char *
cuserid (char *src)
{
  if (!src)
    return getlogin ();

  strcpy (src, getlogin ());
  return src;
}

const char *
cygheap_user::ontherange (homebodies what, struct passwd *pw)
{
  LPUSER_INFO_3 ui = NULL;
  WCHAR wuser[UNLEN + 1];
  NET_API_STATUS ret;
  char homepath_env_buf[CYG_MAX_PATH + 1];
  char homedrive_env_buf[3];
  char *newhomedrive = NULL;
  char *newhomepath = NULL;


  debug_printf ("what %d, pw %p", what, pw);
  if (what == CH_HOME)
    {
      char *p;
      if (homedrive)
	newhomedrive = homedrive;
      else if ((p = getenv ("HOMEDRIVE")))
	newhomedrive = p;

      if (homepath)
	newhomepath = homepath;
      else if ((p = getenv ("HOMEPATH")))
	newhomepath = p;

      if ((p = getenv ("HOME")))
	debug_printf ("HOME is already in the environment %s", p);
      else
	{
	  if (pw && pw->pw_dir && *pw->pw_dir)
	    {
	      debug_printf ("Set HOME (from /etc/passwd) to %s", pw->pw_dir);
	      setenv ("HOME", pw->pw_dir, 1);
	    }
	  else if (!newhomedrive || !newhomepath)
	    setenv ("HOME", "/", 1);
	  else
	    {
	      char home[CYG_MAX_PATH];
	      char buf[CYG_MAX_PATH + 1];
	      strcpy (buf, newhomedrive);
	      strcat (buf, newhomepath);
	      cygwin_conv_to_full_posix_path (buf, home);
	      debug_printf ("Set HOME (from HOMEDRIVE/HOMEPATH) to %s", home);
	      setenv ("HOME", home, 1);
	    }
	}
    }

  if (what != CH_HOME && homepath == NULL && newhomepath == NULL)
    {
      if (!pw)
	pw = internal_getpwnam (name ());
      if (pw && pw->pw_dir && *pw->pw_dir)
	cygwin_conv_to_full_win32_path (pw->pw_dir, homepath_env_buf);
      else
	{
	  homepath_env_buf[0] = homepath_env_buf[1] = '\0';
	  if (logsrv ())
	    {
	      WCHAR wlogsrv[INTERNET_MAX_HOST_NAME_LENGTH + 3];
	      sys_mbstowcs (wlogsrv, logsrv (),
			    sizeof (wlogsrv) / sizeof (*wlogsrv));
	     sys_mbstowcs (wuser, winname (), sizeof (wuser) / sizeof (*wuser));
	      if (!(ret = NetUserGetInfo (wlogsrv, wuser, 3, (LPBYTE *) &ui)))
		{
		  sys_wcstombs (homepath_env_buf, ui->usri3_home_dir, CYG_MAX_PATH);
		  if (!homepath_env_buf[0])
		    {
		      sys_wcstombs (homepath_env_buf, ui->usri3_home_dir_drive,
				    CYG_MAX_PATH);
		      if (homepath_env_buf[0])
			strcat (homepath_env_buf, "\\");
		      else
			cygwin_conv_to_full_win32_path ("/", homepath_env_buf);
		    }
		}
	    }
	  if (ui)
	    NetApiBufferFree (ui);
	}

      if (homepath_env_buf[1] != ':')
	{
	  newhomedrive = almost_null;
	  newhomepath = homepath_env_buf;
	}
      else
	{
	  homedrive_env_buf[0] = homepath_env_buf[0];
	  homedrive_env_buf[1] = homepath_env_buf[1];
	  homedrive_env_buf[2] = '\0';
	  newhomedrive = homedrive_env_buf;
	  newhomepath = homepath_env_buf + 2;
	}
    }

  if (newhomedrive && newhomedrive != homedrive)
    cfree_and_set (homedrive, (newhomedrive == almost_null)
			      ? almost_null : cstrdup (newhomedrive));

  if (newhomepath && newhomepath != homepath)
    cfree_and_set (homepath, cstrdup (newhomepath));

  switch (what)
    {
    case CH_HOMEDRIVE:
      return homedrive;
    case CH_HOMEPATH:
      return homepath;
    default:
      return homepath;
    }
}

const char *
cygheap_user::test_uid (char *&what, const char *name, size_t namelen)
{
  if (!what && !issetuid ())
    what = getwinenveq (name, namelen, HEAP_STR);
  return what;
}

const char *
cygheap_user::env_logsrv (const char *name, size_t namelen)
{
  if (test_uid (plogsrv, name, namelen))
    return plogsrv;

  const char *mydomain = domain ();
  const char *myname = winname ();
  if (!mydomain || strcasematch (myname, "SYSTEM"))
    return almost_null;

  char logsrv[INTERNET_MAX_HOST_NAME_LENGTH + 3];
  cfree_and_set (plogsrv, almost_null);
  if (get_logon_server (mydomain, logsrv, NULL))
    plogsrv = cstrdup (logsrv);
  return plogsrv;
}

const char *
cygheap_user::env_domain (const char *name, size_t namelen)
{
  if (pwinname && test_uid (pdomain, name, namelen))
    return pdomain;

  char username[UNLEN + 1];
  DWORD ulen = sizeof (username);
  char userdomain[DNLEN + 1];
  DWORD dlen = sizeof (userdomain);
  SID_NAME_USE use;

  cfree_and_set (pwinname, almost_null);
  cfree_and_set (pdomain, almost_null);
  if (!LookupAccountSid (NULL, sid (), username, &ulen,
			 userdomain, &dlen, &use))
    __seterrno ();
  else
    {
      pwinname = cstrdup (username);
      pdomain = cstrdup (userdomain);
    }
  return pdomain;
}

const char *
cygheap_user::env_userprofile (const char *name, size_t namelen)
{
  if (test_uid (puserprof, name, namelen))
    return puserprof;

  char userprofile_env_buf[CYG_MAX_PATH + 1];
  cfree_and_set (puserprof, almost_null);
  /* FIXME: Should this just be setting a puserprofile like everything else? */
  const char *myname = winname ();
  if (myname && strcasematch (myname, "SYSTEM")
      && get_registry_hive_path (sid (), userprofile_env_buf))
    puserprof = cstrdup (userprofile_env_buf);

  return puserprof;
}

const char *
cygheap_user::env_homepath (const char *name, size_t namelen)
{
  return ontherange (CH_HOMEPATH);
}

const char *
cygheap_user::env_homedrive (const char *name, size_t namelen)
{
  return ontherange (CH_HOMEDRIVE);
}

const char *
cygheap_user::env_name (const char *name, size_t namelen)
{
  if (!test_uid (pwinname, name, namelen))
    (void) domain ();
  return pwinname;
}

char *
pwdgrp::next_str (char c)
{
  char *res = lptr;
  lptr = strechr (lptr, c);
  if (*lptr)
    *lptr++ = '\0';
  return res;
}

bool
pwdgrp::next_num (unsigned long& n)
{
  char *p = next_str (':');
  char *cp;
  n = strtoul (p, &cp, 10);
  return p != cp && !*cp;
}

char *
pwdgrp::add_line (char *eptr)
{
  if (eptr)
    {
      lptr = eptr;
      eptr = strchr (lptr, '\n');
      if (eptr)
	{
	  if (eptr > lptr && eptr[-1] == '\r')
	    eptr[-1] = '\0';
	  else
	    *eptr = '\0';
	  eptr++;
	}
      if (curr_lines >= max_lines)
	{
	  max_lines += 10;
	  *pwdgrp_buf = realloc (*pwdgrp_buf, max_lines * pwdgrp_buf_elem_size);
	}
      if ((this->*parse) ())
	curr_lines++;
    }
  return eptr;
}

void
pwdgrp::load (const char *posix_fname)
{
  const char *res;
  static const char failed[] = "failed";
  static const char succeeded[] = "succeeded";

  if (buf)
    free (buf);
  buf = NULL;
  curr_lines = 0;

  pc.check (posix_fname);
  etc_ix = etc::init (etc_ix, pc);

  paranoid_printf ("%s", posix_fname);

  if (pc.error || !pc.exists () || !pc.isdisk () || pc.isdir ())
    {
      paranoid_printf ("strange path_conv problem");
      res = failed;
    }
  else
    {
      HANDLE fh = CreateFile (pc, GENERIC_READ, wincap.shared (), NULL,
			      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
      if (fh == INVALID_HANDLE_VALUE)
	{
	  paranoid_printf ("%s CreateFile failed, %E");
	  res = failed;
	}
      else
	{
	  DWORD size = GetFileSize (fh, NULL), read_bytes;
	  buf = (char *) malloc (size + 1);
	  if (!ReadFile (fh, buf, size, &read_bytes, NULL))
	    {
	      paranoid_printf ("ReadFile failed, %E");
	      CloseHandle (fh);
	      if (buf)
		free (buf);
	      buf = NULL;
	      res = failed;
	    }
	  else
	    {
	      CloseHandle (fh);
	      buf[read_bytes] = '\0';
	      char *eptr = buf;
	      while ((eptr = add_line (eptr)))
		continue;
	      debug_printf ("%s curr_lines %d", posix_fname, curr_lines);
	      res = succeeded;
	    }
	}
    }

  debug_printf ("%s load %s", posix_fname, res);
  initialized = true;
  return;
}
