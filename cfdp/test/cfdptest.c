/*

	cfdptest.c:	CFDP test shell program.

									*/
/*	Copyright (c) 2009, California Institute of Technology.		*/
/*	All rights reserved.						*/
/*	Author: Scott Burleigh, Jet Propulsion Laboratory		*/

#include "cfdp.h"

typedef struct
{
	CfdpHandler		faultHandlers[16];
	CfdpNumber		destinationEntityNbr;
	char			sourceFileNameBuf[256];
	char			*sourceFileName;
	char			destFileNameBuf[256];
	char			*destFileName;
	BpUtParms		utParms;
	MetadataList		msgsToUser;
	MetadataList		fsRequests;
	CfdpTransactionId	transactionId;
} CfdpReqParms;

static void	handleQuit()
{
	PUTS("Please enter command 'q' to stop the program.");
}

static void	printUsage()
{
	PUTS("Valid commands are:");
	PUTS("\tq\tQuit");
	PUTS("\th\tHelp");
	PUTS("\t?\tHelp");
	PUTS("\td\tSet destination entity number");
	PUTS("\t   d <destination entity number>");
	PUTS("\tf\tSet source file name");
	PUTS("\t   f <source file name>");
	PUTS("\tt\tSet destination file name");
	PUTS("\t   t <destination file name>");
	PUTS("\tl\tSet time-to-live (lifetime, in seconds)");
	PUTS("\t   l <time-to-live>");
	PUTS("\tp\tSet priority");
	PUTS("\t   p <priority: 0, 1, or 2>");
	PUTS("\to\tSet ordinal (sub-priority within priority 2)");
	PUTS("\t   o <ordinal>");
	PUTS("\tm\tSet transmission mode");
	PUTS("\t   m <0 = reliable (the default), 1 = unreliable>");
	PUTS("\tr\tAdd filestore request");
	PUTS("\t   r <action code nbr> <first path name> <second path name>");
	PUTS("\t\t\tAction code numbers are:");
	PUTS("\t\t\t\t0 = create file");
	PUTS("\t\t\t\t1 = delete file");
	PUTS("\t\t\t\t2 = rename file");
	PUTS("\t\t\t\t3 = append file");
	PUTS("\t\t\t\t4 = replace file");
	PUTS("\t\t\t\t5 = create directory");
	PUTS("\t\t\t\t6 = remove directory");
	PUTS("\t\t\t\t7 = deny file");
	PUTS("\t\t\t\t8 = deny directory");
	PUTS("\tu\tAdd message to user");
	PUTS("\t   u '<message text>'");
	PUTS("\t&\tSend file per specified parameters");
	PUTS("\t   &");
	PUTS("\t^\tCancel the current file transmission");
	PUTS("\t   ^");
	PUTS("\t%\tSuspend the current file transmission");
	PUTS("\t   %");
	PUTS("\t$\tResume the current file transmission");
	PUTS("\t   $");
	PUTS("\t#\tReport on the current file transmission");
	PUTS("\t   #");
}

static void	setDestinationEntityNbr(int tokenCount, char **tokens,
			CfdpNumber *destinationEntityNbr)
{
	unsigned long	entityId;

	if (tokenCount != 2)
	{
		PUTS("What's the destination entity number?");
		return;
	}

	entityId = strtol(tokens[1], NULL, 0);
	cfdp_compress_number(destinationEntityNbr, entityId);
}

static void	setSourceFileName(int tokenCount, char **tokens,
			char *sourceFileNameBuf, char **sourceFileName)
{
	if (tokenCount != 2)
	{
		PUTS("What's the source file name?");
		return;
	}

	isprintf(sourceFileNameBuf, 256, "%.255s", tokens[1]);
	*sourceFileName = sourceFileNameBuf;
}

static void	setDestFileName(int tokenCount, char **tokens,
			char *destFileNameBuf, char **destFileName)
{
	if (tokenCount != 2)
	{
		PUTS("What's the destination file name?");
		return;
	}

	isprintf(destFileNameBuf, 256, "%.255s", tokens[1]);
	*destFileName = destFileNameBuf;
}

static void	setClassOfService(int tokenCount, char **tokens,
			BpUtParms *utParms)
{
	unsigned long	priority;

	if (tokenCount != 2)
	{
		PUTS("What's the priority?");
		return;
	}

	priority = strtol(tokens[1], NULL, 0);
	utParms->classOfService = priority;
}

static void	setOrdinal(int tokenCount, char **tokens, BpUtParms *utParms)
{
	unsigned long	ordinal;

	if (tokenCount != 2)
	{
		PUTS("What's the ordinal?");
		return;
	}

	ordinal = strtol(tokens[1], NULL, 0);
	utParms->extendedCOS.ordinal = ordinal;
}

static void	setMode(int tokenCount, char **tokens, BpUtParms *utParms)
{
	unsigned long	mode;

	if (tokenCount != 2)
	{
		PUTS("What's the mode?");
		return;
	}

	mode = (strtol(tokens[1], NULL, 0) == 0 ? 0 : 1);
	if (mode == 1)
	{
		utParms->extendedCOS.flags |= BP_BEST_EFFORT;
		utParms->custodySwitch = NoCustodyRequested;
	}
	else
	{
		utParms->extendedCOS.flags &= (~BP_BEST_EFFORT);
		utParms->custodySwitch = SourceCustodyRequired;
	}
}

static void	setCriticality(int tokenCount, char **tokens,
			BpUtParms *utParms)
{
	unsigned long	criticality;

	if (tokenCount != 2)
	{
		PUTS("What's the criticality?");
		return;
	}

	criticality = (strtol(tokens[1], NULL, 0) == 0 ? 0 : 1);
	if (criticality == 1)
	{
		utParms->extendedCOS.flags |= BP_MINIMUM_LATENCY;
	}
	else
	{
		utParms->extendedCOS.flags &= (~BP_MINIMUM_LATENCY);
	}
}

static void	setTTL(int tokenCount, char **tokens, BpUtParms *utParms)
{
	unsigned long	TTL;

	if (tokenCount != 2)
	{
		PUTS("What's the TTL?");
		return;
	}

	TTL = strtol(tokens[1], NULL, 0);
	utParms->lifespan = TTL;
}

static void	addMsgToUser(int tokenCount, char **tokens,
			MetadataList *msgsToUser)
{
	if (tokenCount != 2)
	{
		PUTS("What's the message text?");
		return;
	}

	if (*msgsToUser == 0)
	{
		*msgsToUser = cfdp_create_usrmsg_list();
	}

	cfdp_add_usrmsg(*msgsToUser, (unsigned char *) tokens[1],
			strlen(tokens[1]) + 1);
}

static void	addFilestoreRequest(int tokenCount, char **tokens,
			MetadataList *fsRequests)
{
	CfdpAction	action;
	char		*firstPathName = NULL;
	char		*secondPathName = NULL;

	switch (tokenCount)
	{
		case 4:
			secondPathName = tokens[3];

			/*	Intentional fall-through to next case.	*/

		case 3:
			firstPathName = tokens[2];
			action = atoi(tokens[1]);
			break;

		default:
			PUTS("Syntax: <action code> <1st name> [<2nd name>]");
			return;
	}

	if (*fsRequests == 0)
	{
		*fsRequests = cfdp_create_fsreq_list();
	}

	cfdp_add_fsreq(*fsRequests, action, firstPathName, secondPathName);
}

static int	processLine(char *line, int lineLength, CfdpReqParms *parms)
{
	int	tokenCount;
	char	*cursor;
	int	i;
	char	*tokens[9];

	tokenCount = 0;
	for (cursor = line, i = 0; i < 9; i++)
	{
		if (*cursor == '\0')
		{
			tokens[i] = NULL;
		}
		else
		{
			findToken(&cursor, &(tokens[i]));
			tokenCount++;
		}
	}

	if (tokenCount == 0)
	{
		return 0;
	}

	/*	Skip over any trailing whitespace.			*/

	while (isspace((int) *cursor))
	{
		cursor++;
	}

	/*	Make sure we've parsed everything.			*/

	if (*cursor != '\0')
	{
		PUTS("Too many tokens.");
		return 0;
	}

	/*	Have parsed the command.  Now execute it.		*/

	switch (*(tokens[0]))		/*	Command code.		*/
	{
		case 0:			/*	Empty line.		*/
			return 0;

		case '?':
		case 'h':
			printUsage();
			return 0;

		case 'd':
			setDestinationEntityNbr(tokenCount, tokens,
					&(parms->destinationEntityNbr));
			return 0;

		case 'f':
			setSourceFileName(tokenCount, tokens,
					parms->sourceFileNameBuf,
					&(parms->sourceFileName));
			return 0;

		case 't':
			setDestFileName(tokenCount, tokens,
					parms->destFileNameBuf,
					&(parms->destFileName));
			return 0;

		case 'l':
			setTTL(tokenCount, tokens, &(parms->utParms));
			return 0;

		case 'p':
			setClassOfService(tokenCount, tokens,
					&(parms->utParms));
			return 0;

		case 'o':
			setOrdinal(tokenCount, tokens, &(parms->utParms));
			return 0;

		case 'm':
			setMode(tokenCount, tokens, &(parms->utParms));
			return 0;

		case 'c':
			setCriticality(tokenCount, tokens, &(parms->utParms));
			return 0;

		case 'u':
			addMsgToUser(tokenCount, tokens, &(parms->msgsToUser));
			return 0;

		case 'r':
			addFilestoreRequest(tokenCount, tokens,
					&(parms->fsRequests));
			return 0;

		case '&':
			if (cfdp_put(&(parms->destinationEntityNbr),
					sizeof(BpUtParms),
					(unsigned char *) &(parms->utParms),
					parms->sourceFileName,
					parms->destFileName, NULL,
					parms->faultHandlers, 0, NULL,
					parms->msgsToUser,
					parms->fsRequests,
					&(parms->transactionId)) < 0)
			{
				putErrmsg("Can't put FDU.", NULL);
				return -1;
			}

			parms->msgsToUser = 0;
			parms->fsRequests = 0;
			return 0;

		case '^':
			if (cfdp_cancel(&(parms->transactionId)) < 0)
			{
				putErrmsg("Can't cancel transaction.", NULL);
				return -1;
			}

			return 0;

		case '%':
			if (cfdp_suspend(&(parms->transactionId)) < 0)
			{
				putErrmsg("Can't suspend transaction.", NULL);
				return -1;
			}

			return 0;

		case '$':
			if (cfdp_resume(&(parms->transactionId)) < 0)
			{
				putErrmsg("Can't resume transaction.", NULL);
				return -1;
			}

			return 0;

		case '#':
			if (cfdp_report(&(parms->transactionId)) < 0)
			{
				putErrmsg("Can't report transaction.", NULL);
				return -1;
			}

			return 0;

		case 'q':
			return -1;	/*	End program.		*/

		default:
			PUTS("Invalid command.  Enter '?' for help.");
			return 0;
	}
}

static void	*handleEvents(void *parm)
{
	int			*running = (int *) parm;
	char			*eventTypes[] =	{
					"no event",
					"transaction started",
					"EOF sent",
					"transaction finished",
					"metadata received",
					"file data segment received",
					"EOF received",
					"suspended",
					"resumed",
					"transaction report",
					"fault",
					"abandoned"
						};
	CfdpEventType		type;
	time_t			time;
	int			reqNbr;
	CfdpTransactionId	transactionId;
	char			sourceFileNameBuf[256];
	char			destFileNameBuf[256];
	unsigned int		fileSize;
	MetadataList		messagesToUser;
	unsigned int		offset;
	unsigned int		length;
	CfdpCondition		condition;
	unsigned int		progress;
	CfdpFileStatus		fileStatus;
	CfdpDeliveryCode	deliveryCode;
	CfdpTransactionId	originatingTransactionId;
	char			statusReportBuf[256];
	MetadataList		filestoreResponses;
	unsigned char		usrmsgBuf[256];
	CfdpAction		action;
	int			status;
	char			firstPathName[256];
	char			secondPathName[256];
	char			msgBuf[256];

	while (*running)
	{
		if (cfdp_get_event(&type, &time, &reqNbr, &transactionId,
				sourceFileNameBuf, destFileNameBuf,
				&fileSize, &messagesToUser, &offset, &length,
				&condition, &progress, &fileStatus,
				&deliveryCode, &originatingTransactionId,
				statusReportBuf, &filestoreResponses) < 0)
		{
			putErrmsg("Failed getting CFDP event.", NULL);
			return NULL;
		}

		if (type == CfdpNoEvent)
		{
			continue;	/*	Interrupted.		*/
		}

		printf("\nEvent: type %d '%s'.\n", type,
				(type > 0 && type < 12) ? eventTypes[type]
				: "(unknown)");
		if (type == CfdpReportInd)
		{
			printf("Report '%s'\n", statusReportBuf);
		}

		while (messagesToUser)
		{
			switch (cfdp_get_usrmsg(&messagesToUser, usrmsgBuf,
					(int *) &length))
			{
			case -1:
				putErrmsg("Failed getting user msg.", NULL);
				return NULL;

			case 0:
				break;

			default:
				usrmsgBuf[length] = '\0';
				printf("\tMessage '%s'\n", usrmsgBuf);
			}
		}

		while (filestoreResponses)
		{
			switch (cfdp_get_fsresp(&filestoreResponses, &action,
					&status, firstPathName, secondPathName,
					msgBuf))
			{
			case -1:
				putErrmsg("Failed getting FS response.", NULL);
				return NULL;

			case 0:
				break;

			default:
				printf("\tResponse %d %d '%s' '%s' '%s'\n",
						action, status, firstPathName,
						secondPathName, msgBuf);
			}
		}

		printf(": ");
		fflush(stdout);
	}

	return NULL;
}

static int	runCfdptestInteractive()
{
	int		cmdFile;
	char		line[256];
	int		len;
	pthread_t	receiverThread;
	int		running = 1;
	CfdpReqParms	parms;

	/*	Start the receiver thread.				*/

	if (pthread_create(&receiverThread, NULL, handleEvents, &running))
	{
		putSysErrmsg("cfdptest can't create receiver thread", NULL);
		return 1;
	}

	memset((char *) &parms, 0, sizeof(CfdpReqParms));
	cfdp_compress_number(&parms.destinationEntityNbr, 0);
	parms.utParms.lifespan = 86400;
	parms.utParms.classOfService = BP_STD_PRIORITY;
	parms.utParms.custodySwitch = SourceCustodyRequired;
	cmdFile = fileno(stdin);
	isignal(SIGINT, handleQuit);
	while (1)
	{
		printf(": ");
		fflush(stdout);
		if (igets(cmdFile, line, sizeof line, &len) == NULL)
		{
			if (len == 0)
			{
				break;
			}

			putErrmsg("igets failed.", NULL);
			break;			/*	Out of loop.	*/
		}

		if (len == 0)
		{
			continue;
		}

		if (processLine(line, len, &parms))
		{
			break;			/*	Out of loop.	*/
		}
	}

	/*	Stop the receiver thread by interrupting reception.	*/

	running = 0;
	cfdp_interrupt();
	pthread_join(receiverThread, NULL);
	PUTS("Stopping cfdptest.");
	return 0;
}

#if defined (VXWORKS) || defined (RTEMS)
int	cfdptest(int a1, int a2, int a3, int a4, int a5,
		int a6, int a7, int a8, int a9, int a10)
{
	unsigned long	destNode = a1 ? strtol((char *) a1, NULL, 0) : 0;
	char		*sourcePath = (char *) a2;
	char		*destPath = (char *) a3;
	unsigned int	ttl = a4 ? atoi((char *) a4) : 0;
	unsigned int	priority = a5 ? atoi((char *) a5) : 0;
	unsigned int	ordinal = a6 ? atoi((char *) a6) : 0;
	unsigned int	unreliable = a7 ? atoi((char *) a7) : 0;
	unsigned int	critical = a8 ? atoi((char *) a8) : 0;
	int		interactive = 0;
#else
int	main(int argc, char **argv)
{
	unsigned long	destNode = argc > 1 ? strtol(argv[1], NULL, 0) : 0;
	char		*sourcePath = argc > 2 ? argv[2] : NULL;
	char		*destPath = argc > 3 ? argv[3] : NULL;
	unsigned int	ttl = argc > 4 ? atoi(argv[4]) : 0;
	unsigned int	priority = argc > 5 ? atoi(argv[5]) : 0;
	unsigned int	ordinal = argc > 6 ? atoi(argv[6]) : 0;
	unsigned int	unreliable = argc > 7 ? atoi(argv[7]) : 0;
	unsigned int	critical = argc > 8 ? atoi(argv[8]) : 0;
	int		interactive = (argc == 1);
#endif
	int		retval = 0;
	CfdpHandler	faultHandlers[16];
	CfdpNumber	destinationEntityNbr;
	char		sourceFileNameBuf[256] = "";
	char		*sourceFileName = NULL;
	char		destFileNameBuf[256] = "";
	char		*destFileName = NULL;
	BpUtParms	utParms = {	0,
					86400,
					BP_STD_PRIORITY,
					SourceCustodyRequired,
					0,
					0,
					{ 0, 0, 0 }	};
	MetadataList	msgsToUser = 0;
	MetadataList	fsRequests = 0;
	CfdpTransactionId	transactionId;

	if (cfdp_init() < 0)
	{
		putErrmsg("cfdptest can't initialize CFDP.", NULL);
		return 1;
	}

	if (interactive)
	{
#ifndef FSWLOGGER	/*	Need stdin/stdout for interactivity.	*/
		retval = runCfdptestInteractive();
		ionDetach();
#endif
		return retval;
	}

	if (destNode == 0 || sourcePath == NULL || destPath == NULL || ttl == 0)
	{
		ionDetach();
		PUTS("Usage: cfdptest [<destination entity nbr> <source file \
name> <destination file name> [<time-to-live, in seconds> [<priority: 0, 1, 2> \
[<ordinal: 0-254> [<unreliable: 0 or 1> [<critical: 0 or 1>]]]]]]");
		return 0;
	}

	cfdp_compress_number(&destinationEntityNbr, destNode);
	isprintf(sourceFileNameBuf, sizeof sourceFileNameBuf, "%.255s",
			sourcePath);
	sourceFileName = sourceFileNameBuf;
	isprintf(destFileNameBuf, sizeof destFileNameBuf, "%.255s", destPath);
	destFileName = destFileNameBuf;
	utParms.classOfService = priority;
	utParms.extendedCOS.ordinal = ordinal;
	utParms.extendedCOS.flags = 0;
	if (unreliable)
	{
		utParms.extendedCOS.flags |= BP_BEST_EFFORT;
		utParms.custodySwitch = NoCustodyRequested;
	}
	else
	{
		utParms.custodySwitch = SourceCustodyRequired;
	}

	if (critical)
	{
		utParms.extendedCOS.flags |= BP_MINIMUM_LATENCY;
	}

	utParms.lifespan = ttl;
	if (cfdp_put(&destinationEntityNbr, sizeof utParms,
			(unsigned char *) &utParms, sourceFileName,
			destFileName, NULL, faultHandlers, 0, NULL,
			msgsToUser, fsRequests, &transactionId) < 0)
	{
		putErrmsg("Can't put FDU.", NULL);
		retval = 1;
	}

	ionDetach();
	return retval;
}