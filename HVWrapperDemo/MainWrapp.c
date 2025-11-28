/*****************************************************************************/
/*                                                                           */
/*        --- CAEN Engineering Srl - Computing Systems Division ---          */
/*                                                                           */
/*   MAINWRAPP.C                                                             */
/*                                                                           */
/*   June      2000:  Rel. 1.0                                               */
/*   Frebruary 2001:  Rel. 1.1                                               */
/*   September 2002:  Rel. 2.6                                               */
/*   November  2002:  Rel. 2.7                                               */
/*																	         */
/*****************************************************************************/
#include <signal.h>
#ifdef UNIX
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "MainWrapp.h"
#include "console.h"
#include "CAENHVWrapper.h"

#define MAX_CMD_LEN        (80)

/* =========================
   Default CLI configuration
   ========================= */
#define DEFAULT_SYSTEM	SY2527
#define DEFAULT_LINK	LINKTYPE_TCPIP
#define DEFAULT_HOST	"192.168.1.2"
#define DEFAULT_USER	"admin"
#define DEFAULT_PASS	"admin"
#define DEFAULT_SLOT	1

/* ----------------------------------------
   Channels to exclude when using '--ch all'
   Edit the list below to skip channels.
   Example: { 3, 7, 15 }
   ---------------------------------------- */
#define EXCLUDED_CH_COUNT 0
static const unsigned short EXCLUDED_CH[EXCLUDED_CH_COUNT] = { };

static int is_channel_excluded(unsigned short ch)
{
	for(int i = 0; i < EXCLUDED_CH_COUNT; i++)
		if(EXCLUDED_CH[i] == ch)
			return 1;
	return 0;
}

/* Default config file paths (first existing one will be used) */
#define DEFAULT_CONFIG_PATH1 "../config/config.txt"
#define DEFAULT_CONFIG_PATH2 "config.txt"

/* strict token parsers to validate numeric fields (avoid treating headers as data) */
static int parse_ushort_token(const char *s, unsigned short *out)
{
	char *endp = NULL;
	if(s == NULL || *s == '\0') return 0;
	unsigned long v = strtoul(s, &endp, 10);
	if(endp == s || *endp != '\0') return 0;
	if(v > 0xFFFF) return 0;
	*out = (unsigned short)v;
	return 1;
}

static int parse_float_token(const char *s, float *out)
{
	char *endp = NULL;
	if(s == NULL || *s == '\0') return 0;
	double v = strtod(s, &endp);
	if(endp == s || *endp != '\0') return 0;
	*out = (float)v;
	return 1;
}

/* ----------------------------------------
   Config file loader
   Format (whitespace or commas as separators):
     ch#   chName   V0Set   I0Set
   chName is ignored by the program.
   ---------------------------------------- */
static int load_config_file(const char *path, unsigned short **outChList, int *outCount, float **outV0List, float **outI0List)
{
	FILE *fp = fopen(path, "r");
	if(!fp) return -1;

	unsigned short *chVec = NULL;
	float *v0Vec = NULL;
	float *i0Vec = NULL;
	int cap = 0;
	int len = 0;
	char line[1024];

	while(fgets(line, sizeof(line), fp)) {
		/* skip empty/comment lines */
		char *p = line;
		while(*p == ' ' || *p == '\t') p++;
		if(*p == '\0' || *p == '\n' || *p == '#')
			continue;

		/* tokenize */
		const char *delims = " ,\t\r\n";
		char *tok = strtok(line, delims);
		if(!tok) continue;
		unsigned short ch;
		if(!parse_ushort_token(tok, &ch))
			continue; /* header or invalid first token: skip line */

		/* second token: chName (ignored) */
		tok = strtok(NULL, delims);
		if(!tok) continue;

		/* third: V0Set */
		tok = strtok(NULL, delims);
		if(!tok) continue;
		float v0;
		if(!parse_float_token(tok, &v0))
			continue;

		/* fourth: I0Set */
		tok = strtok(NULL, delims);
		if(!tok) continue;
		float i0;
		if(!parse_float_token(tok, &i0))
			continue;

		if(is_channel_excluded((unsigned short)ch))
			continue;

		if(len >= cap) {
			int ncap = (cap == 0 ? 16 : cap * 2);
			unsigned short *nch = (unsigned short*)realloc(chVec, sizeof(unsigned short) * (size_t)ncap);
			float *nv0 = (float*)realloc(v0Vec, sizeof(float) * (size_t)ncap);
			float *ni0 = (float*)realloc(i0Vec, sizeof(float) * (size_t)ncap);
			if(!nch || !nv0 || !ni0) {
				if(nch) chVec = nch;
				if(nv0) v0Vec = nv0;
				if(ni0) i0Vec = ni0;
				fclose(fp);
				free(chVec);
				free(v0Vec);
				free(i0Vec);
				return -2;
			}
			chVec = nch;
			v0Vec = nv0;
			i0Vec = ni0;
			cap = ncap;
		}
		chVec[len] = (unsigned short)ch;
		v0Vec[len] = v0;
		i0Vec[len] = i0;
		len++;
	}
	fclose(fp);

	if(len == 0) {
		free(chVec);
		free(v0Vec);
		free(i0Vec);
		return 0;
	}

	*outChList = chVec;
	*outV0List = v0Vec;
	*outI0List = i0Vec;
	*outCount = len;
	return len;
}

static int load_default_config(unsigned short **outChList, int *outCount, float **outV0List, float **outI0List)
{
	int r = load_config_file(DEFAULT_CONFIG_PATH1, outChList, outCount, outV0List, outI0List);
	if(r >= 0) return r;
	return load_config_file(DEFAULT_CONFIG_PATH2, outChList, outCount, outV0List, outI0List);
}

typedef void (*P_FUN)(void);

typedef struct cmds
			{
				char  cmdName[MAX_CMD_LEN]; /* nome del comando */
				P_FUN pFun;
			} CMD;

static CMD function[] = {
		{ "LIBRARYRELEASE", HVLibSwRel },
        { "LOGIN", HVSystemLogin },
        { "LOGOUT", HVSystemLogout },
        { "GETCHNAME", HVGetChName },
        { "SETCHNAME", HVSetChName }, 
		{ "GETCHPARAMPROP", HVGetChParamProp },
        { "GETCHPARAM", HVGetChParam },
        { "SETCHPARAM", HVSetChParam },
     //   { "TSTBDPRES", HVTstBdPres },
		{ "GETBDPARAMPROP", HVGetBdParamProp },
		{ "GETBDPARAM", HVGetBdParam },
		{ "SETBDPARAM", HVSetBdParam },					// Rel. 2.7
     /*   { "GETGRPCOMP", HVGetGrpComp },
        { "ADDCHTOGRP", HVAddChToGrp },
        { "REMCHFROMGRP", HVRemChFromGrp },*/
        { "GETCRATEMAP", HVGetCrateMap },
	/*	{ "GETSYSCOMP", HVGetSysComp },*/
		{ "GETEXECLIST", HVGetExecList }, 
		{ "GETSYSPROP", HVGetSysProp },
		{ "SETSYSPROP", HVSetSysProp },
		{ "EXECOMMAND", HVExecComm },
	/*	{ "CAENETCOMMAND", HVCaenetComm }, */
        { "NOCOMMAND", HVnoFunction }
                 };

static char  alpha[] =
		{
			"abcdefghijklmnopqrstuv"
		};

HV System[MAX_HVPS];
int loop;

/*****************************************************************************/
/*                                                                           */
/*  COMMANDLIST                                                              */
/*                                                                           */
/*  Aut: A. Morachioli                                                       */
/*                                                                           */
/*****************************************************************************/
static void commandList(void)
{
	unsigned short  nOfSys = 0, nOfCmd = 0, pageSys = 0, 
					i, j, page = 0, row, column;
	int				cmd;

	while( strcmp(function[nOfCmd].cmdName, "NOCOMMAND") )
		nOfCmd++;

	while( 1 )
	{
		clrscr();
		con_puts("       --- Demonstration of use of CAEN HV Wrapper Library --- ");
		gotoxy(1, 3);
		for(i=page*20;(i<(page*20+20))&&(strcmp(function[i].cmdName,"NOCOMMAND"));i++)
		{
			row = 3 + (i - page * 20)%10;
			column = ((i - page * 20) > 9 ? 30 : 1);
			gotoxy(column, row);
			con_printf("[%c] %s", alpha[i], function[i].cmdName);
		}

		for( j = pageSys*10; (j<pageSys*10+10)&&(System[j].ID!=-1); j++ )
		{
			row = 3+(j-pageSys*10)%10;
			gotoxy(60,row);
			con_printf("System[%d]: %d", j, System[j].Handle);
		}

		gotoxy(1, 14);
		con_printf("[r] Loop = %s",loop ? "Yes" : "No");

		gotoxy(1, 15);
		con_printf("[x] Exit \n\n");

		switch(cmd = tolower(con_getch()))
		{
			// Handle future next page command
			//	case "next page"
			//		if( nOfCmd > page*20 + 20 )
			//			page++;
			//		else
			//			page = 0;
			//       break;

			// Handle future next system command
			//  case "next system"
			//		while( System[nOfSys].ID != -1 )
            //			nOfSys++;
			//		if( nOfSys > pageSys*10+10 )
            //			pageSys++;
			//		else
            //			pageSys = 0;
			//        break;

	   case 'r':
	     loop = (loop ? 0 : 1);
	    break;

		case 'x':
			quitProgram();
			break;

	    default:
			if( cmd >= 'a' && cmd < 'a' + nOfCmd )
				(*function[cmd-'a'].pFun)();      
	        break;
      }
  }
}

/*****************************************************************************/
/*                                                                           */
/*  MAIN                                                                     */
/*                                                                           */
/*  Aut: A. Morachioli                                                       */
/*                                                                           */
/*****************************************************************************/
/* ---------------------- */
/* Simple CLI integration */
/* ---------------------- */
typedef struct {
	char	name[64];
	char	value[128];
} cli_param_t;

static int str_ieq(const char *a, const char *b) {
	if(a == NULL || b == NULL) return 0;
	while(*a && *b) {
		char ca = (char)tolower((unsigned char)*a++);
		char cb = (char)tolower((unsigned char)*b++);
		if(ca != cb) return 0;
	}
	return *a == '\0' && *b == '\0';
}

static int parse_system_type(const char *s, CAENHV_SYSTEM_TYPE_t *out) {
	if(s == NULL || out == NULL) return -1;
	if(str_ieq(s, "SY1527")) { *out = SY1527; return 0; }
	if(str_ieq(s, "SY2527")) { *out = SY2527; return 0; }
	if(str_ieq(s, "SY4527")) { *out = SY4527; return 0; }
	if(str_ieq(s, "SY5527")) { *out = SY5527; return 0; }
	if(str_ieq(s, "V65XX"))  { *out = V65XX;  return 0; }
	if(str_ieq(s, "N1470"))  { *out = N1470;  return 0; }
	if(str_ieq(s, "V8100"))  { *out = V8100;  return 0; }
	if(str_ieq(s, "N568E"))  { *out = N568E;  return 0; }
	if(str_ieq(s, "DT55XX")) { *out = DT55XX; return 0; }
	if(str_ieq(s, "DT55XXE")){ *out = DT55XXE;return 0; }
	if(str_ieq(s, "SMARTHV")){ *out = SMARTHV;return 0; }
	if(str_ieq(s, "NGPS"))   { *out = NGPS;   return 0; }
	if(str_ieq(s, "N1068"))  { *out = N1068;  return 0; }
	if(str_ieq(s, "N1168"))  { *out = N1168;  return 0; }
	if(str_ieq(s, "R6060"))  { *out = R6060;  return 0; }
	return -1;
}

static int parse_link_type(const char *s, int *outLinkType) {
	if(s == NULL || outLinkType == NULL) return -1;
	if(str_ieq(s, "tcpip"))      { *outLinkType = LINKTYPE_TCPIP;    return 0; }
	if(str_ieq(s, "rs232"))      { *outLinkType = LINKTYPE_RS232;    return 0; }
	if(str_ieq(s, "caenet"))     { *outLinkType = LINKTYPE_CAENET;   return 0; }
	if(str_ieq(s, "usb"))        { *outLinkType = LINKTYPE_USB;      return 0; }
	if(str_ieq(s, "optlink") || str_ieq(s, "optical") || str_ieq(s, "optical_link")) { *outLinkType = LINKTYPE_OPTLINK; return 0; }
	if(str_ieq(s, "usbvcp") || str_ieq(s, "usb_vcp")) { *outLinkType = LINKTYPE_USB_VCP; return 0; }
	if(str_ieq(s, "usb3"))       { *outLinkType = LINKTYPE_USB3;     return 0; }
	if(str_ieq(s, "a4818"))      { *outLinkType = LINKTYPE_A4818;    return 0; }
	return -1;
}

static int is_flag(const char *s) {
	return (s && s[0] == '-' && s[1] == '-');
}

static void print_cli_usage(const char *prog) {
	fprintf(stderr,
		"Usage (CLI mode): %s --ch 0 1 2 3 \\\n"
		"                  --V0Set 650 --Pw On\n"
		"       (read)     %s --ch 0 1 --IMon\n"
		"       (read)     %s --ch 0 1 --VMon\n"
		"       (read)     %s --ch 0 1 --ChStatus\n"
		"       (read all) %s --ch all --IMon\n"
		"       (read all) %s --ch all --VMon\n"
		"       (read all) %s --ch all --ChStatus\n"
		"       (Pw all)   %s --ch all --Pw On | Off\n"
		"       (Pw all)   %s --ch all --PwOn | --PwOff\n"
		"       (config)   %s --Pw On|Off   (reads per-channel V0Set/I0Set from config)\n"
		"\n"
		"Notes:\n"
		"- Connection is fixed to TCP/IP host 192.168.1.2.\n"
		"- System is fixed to SY2527. Login is fixed to admin/admin. Slot is fixed to 1.\n"
		"- You can provide multiple parameter assignments: any --<ParamName> <value> is applied to all channels.\n"
		"- If no arguments are provided, the interactive ncurses demo UI is started.\n",
		prog ? prog : "HVWrappdemo",
		prog ? prog : "HVWrappdemo",
		prog ? prog : "HVWrappdemo",
		prog ? prog : "HVWrappdemo",
		prog ? prog : "HVWrappdemo",
		prog ? prog : "HVWrappdemo",
		prog ? prog : "HVWrappdemo",
		prog ? prog : "HVWrappdemo");
}

static int run_cli(int argc, char **argv) {
	CAENHV_SYSTEM_TYPE_t sysType = DEFAULT_SYSTEM;
	int linkType = DEFAULT_LINK; /* fixed */
	const char *user = NULL;
	const char *pass = NULL;
	int slot = -1;
	unsigned short *chList = NULL;
	int chCount = 0;
	cli_param_t params[32];
	int paramCount = 0;
	const char *getParam = NULL;
	int chAll = 0;
	const char *configPath = NULL;
	int i;

	for(i = 1; i < argc; i++) {
		if(str_ieq(argv[i], "--help")) {
			print_cli_usage(argv[0]);
			return 1;
		} else if(str_ieq(argv[i], "--system") && i+1 < argc) {
			if(parse_system_type(argv[i+1], &sysType) != 0) {
				fprintf(stderr, "Unknown --system '%s'\n", argv[i+1]);
				return 2;
			}
			i++;
		} else if(str_ieq(argv[i], "--user") && i+1 < argc) {
			user = argv[++i];
		} else if(str_ieq(argv[i], "--pass") && i+1 < argc) {
			pass = argv[++i];
		} else if(str_ieq(argv[i], "--slot") && i+1 < argc) {
			slot = atoi(argv[++i]);
		} else if(str_ieq(argv[i], "--config") && i+1 < argc) {
			configPath = argv[++i];
		} else if(str_ieq(argv[i], "--get") && i+1 < argc) {
			getParam = argv[++i];
		} else if(str_ieq(argv[i], "--IMon")) {
			getParam = "IMon";
		} else if(str_ieq(argv[i], "--VMon")) {
			getParam = "VMon";
		} else if(str_ieq(argv[i], "--ChStatus")) {
			getParam = "ChStatus";
		} else if(str_ieq(argv[i], "--PwOn")) {
			if(paramCount >= (int)(sizeof(params)/sizeof(params[0]))) {
				fprintf(stderr, "Too many parameters specified\n");
				return 2;
			}
			snprintf(params[paramCount].name, sizeof(params[paramCount].name), "%s", "Pw");
			snprintf(params[paramCount].value, sizeof(params[paramCount].value), "%s", "On");
			paramCount++;
		} else if(str_ieq(argv[i], "--PwOff")) {
			if(paramCount >= (int)(sizeof(params)/sizeof(params[0]))) {
				fprintf(stderr, "Too many parameters specified\n");
				return 2;
			}
			snprintf(params[paramCount].name, sizeof(params[paramCount].name), "%s", "Pw");
			snprintf(params[paramCount].value, sizeof(params[paramCount].value), "%s", "Off");
			paramCount++;
		} else if(str_ieq(argv[i], "--ch")) {
			int j = i + 1;
			if(j < argc && !is_flag(argv[j]) && str_ieq(argv[j], "all")) {
				chAll = 1;
				i = j; /* consume 'all' */
			} else {
				int start = j;
				int count = 0;
				while(j < argc && !is_flag(argv[j])) { j++; count++; }
				if(count <= 0) {
					fprintf(stderr, "Expected one or more channel indices after --ch\n");
					return 2;
				}
				chList = (unsigned short*)malloc(sizeof(unsigned short) * (size_t)count);
				if(!chList) {
					fprintf(stderr, "Out of memory\n");
					return 3;
				}
				for(int k = 0; k < count; k++) {
					chList[k] = (unsigned short)atoi(argv[start + k]);
				}
				chCount = count;
				i = j - 1;
			}
		} else if(is_flag(argv[i])) {
			/* Treat as a parameter assignment: --ParamName VALUE */
			const char *flag = argv[i];
			const char *name = flag + 2;
			if(name[0] == '\0') {
				fprintf(stderr, "Invalid flag '%s'\n", flag);
				return 2;
			}
			if(i + 1 >= argc || is_flag(argv[i+1])) {
				fprintf(stderr, "Missing value for parameter '%s'\n", name);
				return 2;
			}
			if(paramCount >= (int)(sizeof(params)/sizeof(params[0]))) {
				fprintf(stderr, "Too many parameters specified\n");
				return 2;
			}
			snprintf(params[paramCount].name, sizeof(params[paramCount].name), "%s", name);
			snprintf(params[paramCount].value, sizeof(params[paramCount].value), "%s", argv[i+1]);
			paramCount++;
			i++;
		} else {
			fprintf(stderr, "Unrecognized argument '%s'\n", argv[i]);
			return 2;
		}
	}

	/* Minimal validation */
	if(!chAll && (chCount <= 0 || chList == NULL)) {
		/* If a Pw setter is present, fallback to config file to build channel list */
		int hasPwSetter = 0;
		for(i = 0; i < paramCount; i++) {
			if(str_ieq(params[i].name, "Pw")) { hasPwSetter = 1; break; }
		}
		if(hasPwSetter) {
			unsigned short *cfgCh = NULL;
			float *cfgV0 = NULL;
			float *cfgI0 = NULL;
			int cfgCount = 0;
			int lr = -1;
			if(configPath) lr = load_config_file(configPath, &cfgCh, &cfgCount, &cfgV0, &cfgI0);
			if(lr < 0) lr = load_default_config(&cfgCh, &cfgCount, &cfgV0, &cfgI0);
			if(lr <= 0) {
				fprintf(stderr, "No channels provided and config not found or empty. Provide --ch or a valid config.\n");
				return 2;
			}
			/* adopt channels from config; free value arrays here and reload later when setting */
			chList = cfgCh;
			chCount = cfgCount;
			free(cfgV0);
			free(cfgI0);
		} else {
			fprintf(stderr, "Missing channels: use --ch <list>\n");
			print_cli_usage(argv[0]);
			return 2;
		}
	}
	if(slot < 0) {
		slot = DEFAULT_SLOT; /* default slot in code */
	}
	if(getParam == NULL && paramCount <= 0) {
		fprintf(stderr, "Nothing to do. Provide setters like --V0Set 650 or a getter like --get IMon\n");
		print_cli_usage(argv[0]);
		return 2;
	}
	if(getParam != NULL && paramCount > 0) {
		fprintf(stderr, "Cannot mix setters and getters in the same call. Use either --get <Param> or set flags.\n");
		return 2;
	}

	/* Prepare connection Arg */
	char connArg[256];
	memset(connArg, 0, sizeof(connArg));
	/* Fixed to TCP/IP 192.168.1.2 */
	snprintf(connArg, sizeof(connArg), "%s", DEFAULT_HOST);

	/* Username/password defaults: match interactive logic */
	char userBuf[64] = {0};
	char passBuf[64] = {0};
	if(user && pass) {
		snprintf(userBuf, sizeof(userBuf), "%s", user);
		snprintf(passBuf, sizeof(passBuf), "%s", pass);
	} else {
		/* For SY4527 / SY5527 / R6060 explicit auth is generally needed; fall back to admin/admin */
		snprintf(userBuf, sizeof(userBuf), "%s", DEFAULT_USER);
		snprintf(passBuf, sizeof(passBuf), "%s", DEFAULT_PASS);
	}

	int handle = -1;
	CAENHVRESULT ret = CAENHV_InitSystem(sysType, linkType, (void*)connArg, userBuf, passBuf, &handle);
	if(ret != CAENHV_OK) {
		fprintf(stderr, "CAENHV_InitSystem failed: %s (code %d)\n", CAENHV_GetError(handle), ret);
		free(chList);
		return (int)ret;
	}

	/* Expand channels if '--ch all' */
	if(chAll) {
		unsigned short NrOfCh = 0;
		int haveChannelCount = 0;

		{
			unsigned short serNumb = 0;
			unsigned char fmwMin = 0, fmwMax = 0;
			char Model[32] = {0}, Descr[64] = {0};
			char *mdl = (char*)Model;
			char *des = (char*)Descr;
			CAENHVRESULT tr = CAENHV_TestBdPresence(handle, (unsigned short)slot,
			                                        &NrOfCh, &mdl, &des,
			                                        &serNumb, &fmwMin, &fmwMax);
			if(tr == CAENHV_OK) {
				haveChannelCount = (NrOfCh > 0);
			} else if(tr != CAENHV_INVALIDPARAMETER && tr != CAENHV_FUNCTIONNOTAVAILABLE) {
				/* 다른 에러는 그대로 리턴 */
				fprintf(stderr, "CAENHV_TestBdPresence failed: %s (code %d)\n",
				        CAENHV_GetError(handle), tr);
				CAENHV_DeinitSystem(handle);
				return (int)tr;
			}
		}


		if(!haveChannelCount) {
			unsigned short nrSlots = 0;
			unsigned short *nrChList = NULL;
			unsigned short *serList = NULL;
			unsigned char *fmwMinList = NULL, *fmwMaxList = NULL;
			char *modelList = NULL, *descList = NULL;

			CAENHVRESULT mr = CAENHV_GetCrateMap(handle,
			                                     &nrSlots,
			                                     &nrChList,
			                                     &modelList,
			                                     &descList,
			                                     &serList,
			                                     &fmwMinList,
			                                     &fmwMaxList);
			if(mr == CAENHV_OK &&
			   slot >= 0 && slot < (int)nrSlots &&
			   nrChList != NULL &&
			   nrChList[slot] > 0) {
				NrOfCh = nrChList[slot];
				haveChannelCount = 1;
			}

			if(nrChList)     CAENHV_Free(nrChList);
			if(modelList)    CAENHV_Free(modelList);
			if(descList)     CAENHV_Free(descList);
			if(serList)      CAENHV_Free(serList);
			if(fmwMinList)   CAENHV_Free(fmwMinList);
			if(fmwMaxList)   CAENHV_Free(fmwMaxList);
		}

		if(!haveChannelCount) {
			unsigned short *cfgCh = NULL;
			float *cfgV0 = NULL;
			float *cfgI0 = NULL;
			int cfgCount = 0;
			int lr = -1;

			if(configPath)
				lr = load_config_file(configPath, &cfgCh, &cfgCount, &cfgV0, &cfgI0);
			if(lr < 0)
				lr = load_default_config(&cfgCh, &cfgCount, &cfgV0, &cfgI0);

			if(lr <= 0 || cfgCh == NULL || cfgCount <= 0) {
				fprintf(stderr, "Unable to determine channel list for '--ch all'. "
				                "Provide explicit --ch list or a valid config file.\n");
				free(cfgCh);
				free(cfgV0);
				free(cfgI0);
				CAENHV_DeinitSystem(handle);
				return 2;
			}

			chList = cfgCh;
			chCount = cfgCount;
			free(cfgV0);
			free(cfgI0);
		} else {
			unsigned short includeCount = 0;
			for(unsigned short c = 0; c < NrOfCh; c++)
				if(!is_channel_excluded(c))
					includeCount++;
			if(includeCount == 0) {
				fprintf(stderr, "No channels to operate on: all channels are excluded by configuration.\n");
				CAENHV_DeinitSystem(handle);
				return 2;
			}
			chList = (unsigned short*)malloc(sizeof(unsigned short) * (size_t)includeCount);
			if(!chList) {
				fprintf(stderr, "Out of memory\n");
				CAENHV_DeinitSystem(handle);
				return 3;
			}
			unsigned short w = 0;
			for(unsigned short c = 0; c < NrOfCh; c++)
				if(!is_channel_excluded(c))
					chList[w++] = c;
			chCount = includeCount;
		}
	}

	int exitCode = 0;
	if(getParam != NULL) {
		/* Read mode */
		unsigned long type = 0;
		CAENHVRESULT pr = CAENHV_GetChParamProp(handle, (unsigned short)slot, chList[0], getParam, "Type", &type);
		if(pr != CAENHV_OK) {
			fprintf(stderr, "GetChParamProp('%s','Type') failed: %s (code %d)\n", getParam, CAENHV_GetError(handle), pr);
			exitCode = (int)pr;
		} else {
			if(type == PARAM_TYPE_NUMERIC) {
				float *vals = (float*)malloc(sizeof(float) * (size_t)chCount);
				if(!vals) {
					fprintf(stderr, "Out of memory\n");
					exitCode = 3;
				} else {
					CAENHVRESULT gr = CAENHV_GetChParam(handle, (unsigned short)slot, getParam, (unsigned short)chCount, chList, vals);
					if(gr != CAENHV_OK) {
						fprintf(stderr, "GetChParam('%s') failed: %s (code %d)\n", getParam, CAENHV_GetError(handle), gr);
						exitCode = (int)gr;
					} else {
						for(int k = 0; k < chCount; k++) {
							printf("Slot %d  Ch %d  %s = %.6f\n", slot, chList[k], getParam, (double)vals[k]);
						}
					}
					free(vals);
				}
			} else {
				unsigned long *vals = (unsigned long*)malloc(sizeof(unsigned long) * (size_t)chCount);
				if(!vals) {
					fprintf(stderr, "Out of memory\n");
					exitCode = 3;
				} else {
					CAENHVRESULT gr = CAENHV_GetChParam(handle, (unsigned short)slot, getParam, (unsigned short)chCount, chList, vals);
					if(gr != CAENHV_OK) {
						fprintf(stderr, "GetChParam('%s') failed: %s (code %d)\n", getParam, CAENHV_GetError(handle), gr);
						exitCode = (int)gr;
					} else {
						for(int k = 0; k < chCount; k++) {
							printf("Slot %d  Ch %d  %s = %lu\n", slot, chList[k], getParam, vals[k]);
						}
					}
					free(vals);
				}
			}
		}
	} else {
		/* Set mode */
		/* If turning power On/Off and channels came from config, apply V0Set/I0Set per-channel from config first */
		{
			int hasPwSetter2 = 0;
			for(int pi = 0; pi < paramCount; pi++) if(str_ieq(params[pi].name, "Pw")) { hasPwSetter2 = 1; break; }
			if(hasPwSetter2 && !chAll) {
				unsigned short *cfgCh = NULL;
				float *cfgV0 = NULL;
				float *cfgI0 = NULL;
				int cfgCount = 0;
				int lr = -1;
				if(configPath) lr = load_config_file(configPath, &cfgCh, &cfgCount, &cfgV0, &cfgI0);
				if(lr < 0) lr = load_default_config(&cfgCh, &cfgCount, &cfgV0, &cfgI0);
				if(lr > 0) {
					for(int idx = 0; idx < cfgCount; idx++) {
						unsigned short oneCh = cfgCh[idx];
						float v0 = cfgV0[idx];
						float i0 = cfgI0[idx];
						CAENHVRESULT sr1 = CAENHV_SetChParam(handle, (unsigned short)slot, "V0Set", 1, &oneCh, &v0);
						if(sr1 != CAENHV_OK) {
							fprintf(stderr, "SetChParam('V0Set', %.3f) ch %u failed: %s (code %d)\n", (double)v0, oneCh, CAENHV_GetError(handle), sr1);
							exitCode = (int)sr1;
						}
						CAENHVRESULT sr2 = CAENHV_SetChParam(handle, (unsigned short)slot, "I0Set", 1, &oneCh, &i0);
						if(sr2 != CAENHV_OK) {
							fprintf(stderr, "SetChParam('I0Set', %.3f) ch %u failed: %s (code %d)\n", (double)i0, oneCh, CAENHV_GetError(handle), sr2);
							exitCode = (int)sr2;
						}
					}
				}
				free(cfgCh);
				free(cfgV0);
				free(cfgI0);
			}
		}
		for(i = 0; i < paramCount; i++) {
			unsigned long type = 0;
			CAENHVRESULT pr = CAENHV_GetChParamProp(handle, (unsigned short)slot, chList[0], params[i].name, "Type", &type);
			if(pr != CAENHV_OK) {
				fprintf(stderr, "GetChParamProp('%s','Type') failed: %s (code %d)\n", params[i].name, CAENHV_GetError(handle), pr);
				exitCode = (int)pr;
				break;
			}

			if(type == PARAM_TYPE_NUMERIC) {
				float fVal = (float)atof(params[i].value);
				CAENHVRESULT sr = CAENHV_SetChParam(handle, (unsigned short)slot, params[i].name, (unsigned short)chCount, chList, &fVal);
				if(sr != CAENHV_OK) {
					fprintf(stderr, "SetChParam('%s', %f) failed: %s (code %d)\n", params[i].name, fVal, CAENHV_GetError(handle), sr);
					exitCode = (int)sr;
					break;
				}
				printf("OK: %s = %g applied to %d channel(s)\n", params[i].name, (double)fVal, chCount);
			} else if(type == PARAM_TYPE_ONOFF) {
				unsigned long lVal = 0;
				if(str_ieq(params[i].value, "on")) lVal = 1;
				else if(str_ieq(params[i].value, "off")) lVal = 0;
				else lVal = (unsigned long)strtoul(params[i].value, NULL, 0);
				CAENHVRESULT sr = CAENHV_SetChParam(handle, (unsigned short)slot, params[i].name, (unsigned short)chCount, chList, &lVal);
				if(sr != CAENHV_OK) {
					fprintf(stderr, "SetChParam('%s', %lu) failed: %s (code %d)\n", params[i].name, lVal, CAENHV_GetError(handle), sr);
					exitCode = (int)sr;
					break;
				}
				printf("OK: %s = %lu applied to %d channel(s)\n", params[i].name, lVal, chCount);
			} else {
				/* Default: treat as integer/enum */
				unsigned long lVal = (unsigned long)strtoul(params[i].value, NULL, 0);
				CAENHVRESULT sr = CAENHV_SetChParam(handle, (unsigned short)slot, params[i].name, (unsigned short)chCount, chList, &lVal);
				if(sr != CAENHV_OK) {
					fprintf(stderr, "SetChParam('%s', %lu) failed: %s (code %d)\n", params[i].name, lVal, CAENHV_GetError(handle), sr);
					exitCode = (int)sr;
					break;
				}
				printf("OK: %s = %lu applied to %d channel(s)\n", params[i].name, lVal, chCount);
			}
		}
	}

	CAENHVRESULT dr = CAENHV_DeinitSystem(handle);
	if(dr != CAENHV_OK) {
		fprintf(stderr, "CAENHV_DeinitSystem: %s (code %d)\n", CAENHV_GetError(handle), dr);
		if(exitCode == 0) exitCode = (int)dr;
	}

	free(chList);
	return exitCode;
}

int main(int argc, char **argv)
{
	int   ret;
	char  esc = 0;

    loop = 0;

	/* CLI mode: if args are provided, run non-interactive flow */
	if(argc > 1) {
		return run_cli(argc, argv);
	}

	for( ret = 0; ret < MAX_HVPS ; ret++ )
		System[ret].ID = -1;

	con_init();
	commandList();
	con_end();

	return 0;
}
