#include <pwd.h>
#include <grp.h>
#include <err.h>
#include <shadow.h>
#include <crypt.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "readpassphrase.h"
#include "sessions.h"

#define VERSION "1.4.1"

void getconf(FILE* fp, const char* entry, char* result, size_t len_result) {
	char* line = NULL;
	size_t len = 0;

	fseek(fp, 0, SEEK_SET);

	while (getline(&line, &len, fp) != -1) {
		if (strncmp(entry, line, strlen(entry)) == 0) {
			strtok(line, "=");
			char* token = strtok(NULL, "=");
			if (token) {
				strncpy(result, token, len_result);
				result[strcspn(result, "\n")] = 0;
				free(line);
				return;
			}
		}
	}

	errx(1, "Could not get '%s' entry in config", entry);
}

void runprog(char** program_argv) {
	if (setuid(0) < 0)
		err(1, "Could not setuid");
	if (setgid(0) < 0)
		err(1, "Could not setgid");

	putenv("HOME=/root");

	// NOTE: this does not return when no error occurred.
	execvp(program_argv[0], program_argv);

	err(1, program_argv[0]);
}

int main(int argc, char** argv) {
	char groupname[64], wrong_pw_sleep[64], session_ttl[64], password[128];
	unsigned int sleep_us, tries, ts_ttl;

	if (argc == 1) {
		printf("RootDO version: %s\n\n", VERSION);
		printf("Usage: %s [command]\n", argv[0]);
		return 0;
	}

	if (geteuid() != 0)
		errx(1, "The rdo binary needs to be installed as SUID.");

	int ruid = getuid();
	if (ruid == 0)
		runprog(&argv[1]);

	FILE* fp = fopen("/etc/rdo.conf", "r");

	if (!fp)
		err(1, "Could not open /etc/rdo.conf");

	getconf(fp, "group", groupname, sizeof(groupname));
	getconf(fp, "wrong_pw_sleep", wrong_pw_sleep, sizeof(wrong_pw_sleep));
	getconf(fp, "session_ttl", session_ttl, sizeof(session_ttl));
	sleep_us = atoi(wrong_pw_sleep) * 1000;
	ts_ttl = atoi(session_ttl) * 60;

	fclose(fp);

	if (getsession(getppid(), ts_ttl, ruid) == 0)
		runprog(&argv[1]);

	struct passwd* pw = getpwuid(ruid);
	if (!pw) {
		if (errno == 0)
			errx(1, "No user with UID %d", ruid);
		else
			err(1, "Could not get user info");
	}

	struct group* current_group_entry = getgrent();
	while (current_group_entry) {
		if (strcmp(current_group_entry->gr_name, groupname) == 0)
			break;
		current_group_entry = getgrent();
	}

	if (!current_group_entry)
		errx(1, "The group '%s' does not exist.", groupname);
	
	char* current_member = current_group_entry->gr_mem[0];
	for (int i = 1; current_member; i++) {
		if (strcmp(current_member, pw->pw_name) == 0)
			break;
		current_member = current_group_entry->gr_mem[i];
	}

	if (!current_member)
		errx(1, "You are not allowed to execute rdo.");

	struct spwd* shadowEntry = getspnam(pw->pw_name);

	if (!shadowEntry || !shadowEntry->sp_pwdp)
		err(1, "Could not get shadow entry");

	tries = 0;
	while (tries < 3) {
		if (!readpassphrase("(rdo) Password: ", password, sizeof(password)))
			err(1, "Could not get passphrase");

		char* hashed_pw = crypt(password, shadowEntry->sp_pwdp);
		memset(password, 0, sizeof(password));
		
		if (!hashed_pw)
			errx(1, "Could not hash password, does your user have a password?");

		if (strcmp(shadowEntry->sp_pwdp, hashed_pw) == 0) {
			setsession(getppid(), ts_ttl, ruid);
			runprog(&argv[1]);
		}

		usleep(sleep_us);
		fprintf(stderr, "Wrong password.\n");
		tries++;
	}
	errx(1, "Too many wrong password attempts.");
	return 1;
}
