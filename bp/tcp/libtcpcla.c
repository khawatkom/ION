/*
	libtcpcla.c:	common functions for BP TCP-based
			convergence-layer daemons.

	Author: Scott Burleigh, JPL

	Copyright (c) 2006, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
	
									*/
#include "tcpcla.h"

int	tcpDelayEnabled = 0;
int	tcpDelayNsecPerByte = 0;
int 	tcpDesiredKeepAlivePeriod = 0;


/*	*	*	Sender functions	*	*	*	*/

void	handleConnectionLoss()
{
	signal(SIGPIPE, handleConnectionLoss);
}

int	connectToCLI(struct sockaddr *sn, int *sock)
{
	*sock = -1;
	if (sn == NULL)
	{
		return -1;	/*	Silently give up on connection.	*/
	}

	*sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (*sock < 0)
	{
		putSysErrmsg("CLO can't open TCP socket", NULL);
		return -1;
	}

	if (connect(*sock, sn, sizeof(struct sockaddr)) < 0)
	{
		close(*sock);
		*sock = -1;
		putSysErrmsg("CLO can't connect to TCP socket", NULL);
		return -1;
	}

	return 0;
}

int	sendBytesByTCP(int *bundleSocket, char *from, int length)
{
	int	bytesWritten;

	while (1)	/*	Continue until not interrupted.		*/
	{
		bytesWritten = write(*bundleSocket, from, length);
		if (bytesWritten < 0)
		{
			switch (errno)
			{
			case EINTR:	/*	Interrupted; retry.	*/
				continue;

			case EPIPE:	/*	Lost connection.	*/
			case EBADF:
			case ETIMEDOUT:
			case ECONNRESET:
				close(*bundleSocket);
				*bundleSocket = -1;
			}

			putSysErrmsg("CLO write() error on socket", NULL);
		}

		return bytesWritten;
	}
}

static int	sendZcoByTCP(int *bundleSocket, unsigned int bundleLength,
			Object bundleZco, unsigned char *buffer,
			unsigned int tcpclSegmentHeaderLength)
{
	Sdr		sdr = getIonsdr();
	int		totalBytesSent = 0;
	unsigned int	bytesRemaining = bundleLength;
	unsigned int	bytesBuffered = tcpclSegmentHeaderLength;
	ZcoReader	reader;
	int		bytesToLoad;
	int		bytesLoaded;
	int		bytesToSend;
	char		*from;
	int		bytesSent;

	zco_start_transmitting(sdr, bundleZco, &reader);
	while (bytesRemaining > 0)
	{
		bytesToLoad = TCPCLA_BUFSZ - bytesBuffered;
		if (bytesToLoad > bytesRemaining)
		{
			bytesToLoad = bytesRemaining;
		}

		sdr_begin_xn(sdr);
		bytesLoaded = zco_transmit(sdr, &reader, bytesToLoad,
				(char *) buffer + bytesBuffered);
		if (sdr_end_xn(sdr) < 0)
		{
			putErrmsg("Can't issue from ZCO.", NULL);
			return -1;
		}

		if (bytesLoaded != bytesToLoad)		/*	Done.	*/
		{
			putErrmsg("ZCO length error.", NULL);
			return -1;
		}

		bytesToSend = bytesBuffered + bytesLoaded;
		bytesRemaining -= bytesToSend;
		from = (char *) buffer;
		while (bytesToSend > 0)
		{
			bytesSent = sendBytesByTCP(bundleSocket, from,
					bytesToSend);
			if (bytesSent < 0)
			{
				if (bpHandleXmitFailure(bundleZco))
				{
					putErrmsg("Can't handle xmit failure.",
							NULL);
					return -1;
				}

				if (*bundleSocket == -1)
				{
					/*	Just lost connection;
					 *	treat as a transient
					 *	anomaly, note the
					 *	incomplete transmission.*/

					writeMemo("[i] Lost TCP connection to \
CLI; restart CLO when connectivity is restored.");
					totalBytesSent = 0;
					bytesRemaining = 0;
					break;	/*	Out of loop.	*/
				}

				/*	Big problem; shut down.		*/

				putErrmsg("Failed to send by TCP.", NULL);
				return -1;
			}

			totalBytesSent += bytesSent;
			from += bytesSent;
			bytesToSend -= bytesSent;
		}

		bytesBuffered = 0;
	}

	zco_stop_transmitting(sdr, &reader);
	sdr_begin_xn(sdr);
	zco_destroy_reference(sdr, bundleZco);
	if (sdr_end_xn(sdr) < 0)
	{
		putErrmsg("Can't destroy bundle ZCO.", NULL);
		return -1;
	}

	return totalBytesSent;
}

int	sendBundleByTCP(struct sockaddr *socketName, int *bundleSocket,
		unsigned int bundleLength, Object bundleZco,
		unsigned char *buffer)
{
	unsigned int	preamble;
	int		bytesToSend;
	char		*from;
	int		bytesSent;

	/*	Connect to CLI as necessary.				*/

	if (*bundleSocket < 0)
	{
		if (connectToCLI(socketName, bundleSocket) < 0)
		{
			/*	Treat I/O error as a transient anomaly.	*/

			return 0;
		}
	}

	/*	Send preamble (length), telling CLI how much to read.	*/

	preamble = htonl(bundleLength);
	bytesToSend = sizeof preamble;
	from = (char *) &preamble;
	while (bytesToSend > 0)
	{
		bytesSent = sendBytesByTCP(bundleSocket, from, bytesToSend);
		if (bytesSent < 0)
		{
			if (*bundleSocket == -1)
			{
				/*	Just lost connection; treat
				 *	as a transient anomaly, note
				 *	incomplete transmission.	*/

				putErrmsg("Lost connection to CLI; restart \
CLO when connectivity is restored.", NULL);
				return 0;
			}

			/*	Big problem; shut down.			*/

			close(*bundleSocket);
			*bundleSocket = -1;
			putErrmsg("Failed to send by TCP.", NULL);
			return -1;
		}

		from += bytesSent;
		bytesToSend -= bytesSent;
	}

	if (bundleLength == 0)		/*	Just a keep-alive.	*/
	{
		return 1;	/*	Impossible length; means "OK".	*/
	}

	/*	Send the bundle itself.					*/

	return sendZcoByTCP(bundleSocket, bundleLength, bundleZco, buffer, 0);
}

/* 	*	*	Send Bundle over TCP Convergence Layer	*	*/

int	sendBundleByTCPCL(struct sockaddr *socketName, int *bundleSocket,
		unsigned int bundleLength, Object bundleZco,
		unsigned char *buffer, int *keepalivePeriod)
{
	int		bytesToSend;
	int		bytesSent;
	Sdnv 		lengthField;
	int		tempBundleSocket;

	/*	Connect to CLI as necessary.				*/

	if (*bundleSocket < 0)
	{
		if (connectToCLI(socketName, &tempBundleSocket) < 0)
		{
			/*	Treat I/O error as a transient anomaly.	*/

			return 0;
		}
		else
		{
			if(sendContactHeader(&tempBundleSocket,buffer) < 0)
			{
				close(tempBundleSocket);
				tempBundleSocket = -1;
				putErrmsg("Could not send Contact Header." \
,NULL);			
				return 0;
			}

			if(receiveContactHeader(&tempBundleSocket,buffer,keepalivePeriod) < 0)
			{
				close(tempBundleSocket);
				tempBundleSocket = -1;
				putErrmsg("Could not receive Contact Header." \
,NULL);
				return 0;
			}
			*bundleSocket = tempBundleSocket;/* The bundle socket is set to 
							    to a positive value, only after
							    the connection is up. Till the 
							    connection is up the tempBundleSocket
							    is used.					*/  

		}
	}

	if (bundleLength == 0)		/*	Just a keep-alive.	*/
	{
		buffer[0] = TCPCLA_TYPE_KEEP_AL << 4;
		bytesSent = sendBytesByTCP(bundleSocket,(char*)buffer,1);
		if(bytesSent < 1)
		{
			if(*bundleSocket == -1)
			{
				putErrmsg("Lost connection to CLI; restart \
CLO when connectivity is restored.",NULL);
				return 0;
			}
			
			close(*bundleSocket);
			*bundleSocket = -1;
			putErrmsg("Failed to send by TCP", NULL);
			return -1;
		}		
		return 1;	/*	Impossible length; means "OK".	*/
	}
	
	/*    	Send the segment Header					*/
	
	buffer[0] = TCPCLA_TYPE_DATA << 4 | 0x03 ;
	/*	The first nibble is 0x1 which means the 
	 *	segment is a data segment. The second
	 *	nibble is 0011, which indicates that
	 *      there is only one segment (assuming
	 *	all the data is sent in one segment).	*/

	encodeSdnv(&lengthField,bundleLength);
	memcpy(buffer + 1,lengthField.text,lengthField.length);
	bytesToSend = 1 + lengthField.length ;
	
	/*	Send the bundle itself.					*/

	return sendZcoByTCP(bundleSocket, bundleLength, bundleZco, buffer,
			bytesToSend);
}

/*	*	*	Receiver functions	*	*	*	*/

int	receiveBytesByTCP(int bundleSocket, char *into, int length)
{
	int	bytesRead;

	bytesRead = read(bundleSocket, into, length);
	switch (bytesRead)
	{
	case -1:
		/*	The read() call may have been interrupted by
		 *	arrival of SIGTERM, in which case reception
		 *	should report that it's time to shut down.	*/

		if (errno == EINTR)	/*	Shutdown.		*/
		{
			return 0;
		}

		putSysErrmsg("CLI read() error on socket", NULL);
		return -1;

	case 0:				/*	Connection closed.	*/
		return 0;

	default:
		return bytesRead;
	}
}

int	receiveBundleByTcp(int bundleSocket, AcqWorkArea *work, char *buffer)
{
	unsigned int	preamble;
	unsigned int	bundleLength = 0;
	int		bytesToReceive;
	char		*into;
	int		bytesReceived;
	int		totalBytesToReceive;
	int		extentSize;

	/*	Receive length of transmitted bundle.			*/

	while (bundleLength == 0)	/*	length 0 is keep-alive	*/
	{
		bytesToReceive = sizeof preamble;
		into = (char *) &preamble;
		while (bytesToReceive > 0)
		{
			bytesReceived = receiveBytesByTCP(bundleSocket, into,
					bytesToReceive);
			if (bytesReceived < 1)
			{
				return bytesReceived;
			}

			into += bytesReceived;
			bytesToReceive -= bytesReceived;
		}

		bundleLength = ntohl(preamble);
	}

	/*	Receive the bundle itself, a buffer's worth at a
	 *	time.							*/

	totalBytesToReceive = bundleLength;
	if (tcpDelayEnabled)	/*	Congestion control testing.	*/
	{
		microsnooze((totalBytesToReceive * tcpDelayNsecPerByte) / 1000);
	}

	while (totalBytesToReceive > 0)
	{
		bytesToReceive = totalBytesToReceive;
		if (bytesToReceive > TCPCLA_BUFSZ)
		{
			bytesToReceive = TCPCLA_BUFSZ;
		}

		extentSize = bytesToReceive;
		into = buffer;
		while (bytesToReceive > 0)
		{
			bytesReceived = receiveBytesByTCP(bundleSocket, into,
					bytesToReceive);
			if (bytesReceived < 1)
			{
				return bytesReceived;
			}

			into += bytesReceived;
			bytesToReceive -= bytesReceived;
		}

		totalBytesToReceive -= extentSize;

		/*	Acquire the received data.			*/

		if (bpContinueAcq(work, buffer, extentSize) < 0)
		{
			return -1;
		}
	}

	return bundleLength;
}

int	receiveBundleByTcpCL(int bundleSocket, AcqWorkArea *work, char *buffer)
{
	int		flags;
	unsigned long	segmentLength;
	long		totalSegmentLength = 0;
	int 		segmentType;
	int 		startSegment = 0;

	
	while(1)
	{
		segmentType = receiveSegmentByTcpCL(bundleSocket,work,buffer,&segmentLength,&flags);
		switch(segmentType)
		{
			case 1:		/*Data Segment*/
				if(flags == 2 && !startSegment)
				{
					startSegment = 1;
					totalSegmentLength = segmentLength;
					break;
				}
				if((flags == 3) && (!startSegment))
				{
					return segmentLength;
				}
				if(startSegment)
				{
					if(flags == 1)
					{
						totalSegmentLength +=segmentLength;
						startSegment = 0;
						return totalSegmentLength;
					}
					else
					{
						totalSegmentLength +=segmentLength;
						break;
					}
				}
				putErrmsg("Flags are not set correctly.",NULL);
				return -1;
			case 2:		/*ACK Segment*/
				putErrmsg("Received ACK_SEGMENT. Unexpected Segment.",NULL);
				break;
			case 3:		/*Refuse Bundle*/
				putErrmsg("Received Refuse Bundle.",NULL);
				break;
			case 4:		/*Keep Alive */
				break;
			case 5:
				putErrmsg("Received Shutdown message.",NULL);
				return 0;
			default:
				return -1;
		}
	}
}


int receiveSegmentByTcpCL(int bundleSocket,AcqWorkArea *work,char *buffer,unsigned long *segmentLength,int *flags)
{
	int 		segmentType;
	int 		length = 0;
	char 		*cursor = buffer;
	int		totalBytesToReceive;
	int	 	bytesToReceive;
	int		bytesReceived;
	int 		extentSize;

	/* Read first byte from received segment */
	if(receiveBytesByTCP(bundleSocket,buffer,1) < 1)
	{
		putErrmsg("Couldnt receive segment header.",NULL);
		return -1;
	}
	segmentType = (buffer[0] & 0xF0) >> 4;
	if(segmentType == 1) 	/* Data Segment	*/
	{
		*flags = (buffer[0] & 0x0F);
		while(length < 10)
		{
			if(receiveBytesByTCP(bundleSocket,buffer+length,1)< 1)
			{
				putErrmsg("Couldn't receive length field.",NULL);
				return -1;
			}
			if((*cursor & 0x80) == 0)	/* Last Byte	*/
			{
				break;
			}
			cursor++;
			length++;
			if(length >= 10)
			{
				putErrmsg("Remote EID length too big.",NULL);
				return -1;
			}
			
		}
		if(decodeSdnv(segmentLength,(unsigned char*)buffer) == 0)
		{
			putErrmsg("The Sdnv doesn't fit into a long variable.",NULL);
			return -1;
		}
		totalBytesToReceive = *segmentLength;
		while (totalBytesToReceive > 0)
		{
			bytesToReceive = totalBytesToReceive;
			if (bytesToReceive > TCPCLA_BUFSZ)
			{
				bytesToReceive = TCPCLA_BUFSZ;
			}

			extentSize = bytesToReceive;
			cursor = buffer;
			while (bytesToReceive > 0)
			{
				bytesReceived = receiveBytesByTCP(bundleSocket, cursor,
						bytesToReceive);
				if (bytesReceived < 1)
				{
					return -1;
				}

				cursor += bytesReceived;
				bytesToReceive -= bytesReceived;
			}

			totalBytesToReceive -= extentSize;

			/*	Acquire the received data.			*/

			if (bpContinueAcq(work, buffer, extentSize) < 0)
			{
				return -1;
			}
		}
	}
	/* Currently none of the other segment types are being handled 		*/
	return segmentType;
}


/*	*	* Creates and sends the contact header	*	*	*/
int sendContactHeader(int *bundleSocket, unsigned char *buffer)
{
	int bytesToSend = 0;
	int bytesSent;
	uint16_t keepaliveIntervalNBO = htons(tcpDesiredKeepAlivePeriod);
	Sdnv eidLength;
	int	custodianEidStringLen;
	char    *custodianEidString;
	int     custodianEidLength;
	char    hostNameBuf[MAXHOSTNAMELEN + 1];

        getNameOfHost(hostNameBuf, MAXHOSTNAMELEN);
	custodianEidStringLen = strlen(hostNameBuf) + 11;
	custodianEidString = MTAKE(custodianEidStringLen);
	CHKERR(custodianEidString);
	isprintf(custodianEidString, custodianEidStringLen, "dtn://%.60s.dtn",
			hostNameBuf);
	custodianEidLength = strlen(custodianEidString);
	if(TCPCLA_BUFSZ < (18 + (custodianEidLength)))
	{
		putErrmsg("Buffer size not big enough for contact header.",
				NULL);
		return -1;
	}

	istrcpy((char*) buffer, TCPCLA_MAGIC, TCPCLA_BUFSZ); 	//Magic='dtn!'
	bytesToSend = TCPCLA_MAGIC_SIZE;
	*(buffer + bytesToSend) =  TCPCLA_ID_VERSION;
	bytesToSend++;
	*(buffer + bytesToSend) = TCPCLA_FLAGS;
	bytesToSend++;
	memcpy(buffer + bytesToSend,&keepaliveIntervalNBO,2);
	bytesToSend +=2;
	//encode local EID length into Sdnv

	encodeSdnv(&eidLength,custodianEidLength);
	memcpy(buffer + bytesToSend, eidLength.text, eidLength.length);
	bytesToSend += eidLength.length;

	//local Eid
	memcpy(buffer + bytesToSend, custodianEidString, custodianEidLength);
	bytesToSend += custodianEidLength;

	MRELEASE(custodianEidString);

	while(bytesToSend > 0)
	{
		bytesSent = sendBytesByTCP(bundleSocket, (char*) buffer, bytesToSend);
		if(bytesSent < 0)
		{
			if(*bundleSocket == -1)
			{
				putErrmsg("Lost connection to CLI; restart \
CLO when connectivity is restored.", NULL);
				return -1;
			}

			/*	Big problem; shut down.			*/

			putErrmsg("Failed to send Contact Header by TCP.", NULL);
			return -1;
		}
		bytesToSend -=bytesSent;
	}
	return 0;
}

/*	*	*Receives Contact Header	*	*	*	*/
int receiveContactHeader(int *bundleSocket, unsigned char *buffer, int *keepalivePeriod)
{
	uint16_t requestedKeepAlive;
	unsigned long remoteEidLength;
	int remoteEidLengthLength = 0; /* Length of SDNV */
	unsigned char * cursor = buffer;
	
	if(receiveBytesByTCP(*bundleSocket, (char*) buffer, TCPCLA_MAGIC_SIZE) < TCPCLA_MAGIC_SIZE)
	{
		putErrmsg("Could not receive contact header magic.",NULL);
		return -1;
	}
	buffer[TCPCLA_MAGIC_SIZE] = '\0';
	if(strcmp((char*)buffer,TCPCLA_MAGIC))
	{
		putErrmsg("Didnt receive contact header magic.",NULL);
		return -1;
	}
	
	/*Reading the version,flags and keep_alive interval from the contact header*/
	if(receiveBytesByTCP(*bundleSocket, (char*) buffer, 4) < 4)
	{
		putErrmsg("Couldn't receive contact header.",NULL);
	}
	
	/*Checking the version*/
	if(buffer[0] > TCPCLA_ID_VERSION)
	{
		/* if the contact header version is higher than 
		 * the current version, then we will try to
		 * interpret the header like we would normally	*/
	}
	else if(buffer[0] < TCPCLA_ID_VERSION)
	{
		putErrmsg("Lower version than current version.",NULL);
		return -1;
	}
	/*checking the flags*/
	switch(buffer[1])
	{
		
		case 0x00:
			/*This means no flags are set*/	
			break;	
		case 0x01: 
			/*This would mean that the request acknowledgement flag is set*/
			break;
		case 0x02:
			/*Reactive fragementation*/
			break;
		case 0x03:
			/*Acknowledgments and fragmentation*/
			break;
		case 0x05:
			/*Case 4 is not possible because if negative ack
			is supported then there must be support for ack
			also 						*/
			break;
		case 0x07:
			/*Supports ack, negative ack and segmentation*/
			break;
		default:
			putErrmsg("Incorrect flags have been set in the header.",NULL);
		
	}
	memcpy(&requestedKeepAlive, &buffer[2],2);
	requestedKeepAlive = ntohs(requestedKeepAlive);
	*keepalivePeriod = MIN(requestedKeepAlive,tcpDesiredKeepAlivePeriod);
	
	while(remoteEidLengthLength < 10)
	{
		if(receiveBytesByTCP(*bundleSocket, (char*) (buffer + remoteEidLengthLength),1) < 1)
		{
			putErrmsg("Problem receiving remote EID length.",NULL);
			return -1;
		}
		if((*cursor & 0x80) == 0)	/* Last Byte	*/
		{
			break;
		}
		cursor++;
		remoteEidLengthLength++;
		if(remoteEidLengthLength >= 10)
		{
			putErrmsg("Remote EID length too big.",NULL);
			return -1;
		}
		
	}
	if(decodeSdnv(&remoteEidLength, (unsigned char *)buffer) == 0)
	{
		putErrmsg("Remote Eid length not SDNV.",NULL);
		return -1;
	}
	
	if(remoteEidLength > TCPCLA_BUFSZ)
	{
		putErrmsg("Remote Eid too long.",NULL);
		return -1;
	}

	if(receiveBytesByTCP(*bundleSocket, (char *)buffer, remoteEidLength) != remoteEidLength)
	{
		putErrmsg("Could receive remote EID.",NULL);
		return -1;
	}

	/* Currently nothing is being done with the remote EID	*/
	return 0;

}


void findVInduct(VInduct ** vduct,char *protocolName)
{
	BpVdb			*bpVdb;
	PsmAddress		elt;
	PsmPartition		bpwm;

	bpVdb = getBpVdb();
	bpwm = getIonwm();
	
	for(elt = sm_list_first(bpwm, bpVdb->inducts); elt ; elt = sm_list_next(bpwm,elt))
	{
		*vduct = (VInduct*)psp(bpwm, sm_list_data(bpwm, elt));
		if(strcmp((*vduct)->protocolName,protocolName) == 0)
		{
			return;
		}
	
	}
	*vduct = NULL;
}

/*	*	* Sends shut down message	*	*	*	*/
int sendShutDownMessage(int *bundleSocket, int reason, int delay)
{
	int 		bytesToSend = 0;
	int 		bytesSent;
	unsigned char 	*buffer;
	uint16_t 	delay_uint16 = delay;

	buffer = MTAKE(SHUT_DN_BUFSZ + 1);
	CHKERR(buffer);
	buffer[0] = TCPCLA_TYPE_SHUT_DN << 4;
	bytesToSend++;
	switch(reason)
	{
	case 0:
		break;
	case 1:
		buffer[0] |= SHUT_DN_REASON_FLAG;
		buffer[1] = SHUT_DN_IDLE_HEX;
		bytesToSend++;
		break;
	case 2:
		buffer[0] |= SHUT_DN_REASON_FLAG;
		buffer[1] = SHUT_DN_VER_HEX;
		bytesToSend++;
		break;
	case 3:
		buffer[0] |= SHUT_DN_REASON_FLAG;
		buffer[1] = SHUT_DN_BUSY_HEX;
		bytesToSend++;
		break;
	default:
		putErrmsg("Unknown reason code!", NULL);
		return -1;
	}
	// if delay is set to 0, then it is interpreted as infinite delay
	// So -1 reperesents that no delay has been set
	if(delay < -1)
	{
		putErrmsg("Delay less than 0!",NULL);
		return -1;
	}
	if(delay > -1)
	{
		buffer[0] |= SHUT_DN_DELAY_FLAG;
		memcpy(buffer + 2,&delay_uint16,2);
		bytesToSend += 2;
	}
	
	while(bytesToSend > 0)
	{
		bytesSent = sendBytesByTCP(bundleSocket, (char*) buffer, bytesToSend);
		if(bytesSent < 0)
		{
			if(*bundleSocket == -1)
			{
				putErrmsg("Lost connection while trying to send \
Shutdown message", NULL);
				return -1;
			}

			/*	Big problem; shut down.			*/

			putErrmsg("Failed to send  shutdown message.", NULL);
			return -1;
		}
		bytesToSend -=bytesSent;
	}
	return 0;
}
	

