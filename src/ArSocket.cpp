/*
Adept MobileRobots Robotics Interface for Applications (ARIA)
Copyright (C) 2004-2005 ActivMedia Robotics LLC
Copyright (C) 2006-2010 MobileRobots Inc.
Copyright (C) 2011-2015 Adept Technology, Inc.
Copyright (C) 2016-2018 Omron Adept Technologies, Inc.

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published by
     the Free Software Foundation; either version 2 of the License, or
     (at your option) any later version.

     This program is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
     GNU General Public License for more details.

     You should have received a copy of the GNU General Public License
     along with this program; if not, write to the Free Software
     Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA


*/
#include "Aria/ArExport.h"
#include "Aria/ariaOSDef.h"
#include "Aria/ArSocket.h"
#include "Aria/ArLog.h"

#ifndef WIN32
// on Windows gethostbyaddr() and inet_ntoa() definitions are included 
// by headers included in ariaOSDef.h, 
// on non-Windows by these headers:
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netdb.h>
#endif

ArSocket::~ArSocket()
{
  if(myDebug) ArLog::log(ArLog::Normal, "ArSocket(%d): object destroyed", myFD);
  close();
}

AREXPORT const char *ArSocket::toString(Type t)
{
  switch (t) {
  case TCP:
    return "TCP";
  case UDP:
    return "UDP";
  default:
    return "Unknown";
  }
  return "Unknown";

} // end method toString

void ArSocket::internalInit()
{
  myReadStringMutex.setLogName("ArMutex::myReadStringMutex");
  myWriteStringMutex.setLogName("ArMutex::myWriteStringMutex");
  myCloseFunctor = NULL;
  myStringAutoEcho = true;
  myStringEcho = false;
  myStringPosLast = 0;
  myStringPos = 0;
  myStringGotComplete = false;
  myStringBufEmpty[0] = '\0';
  myStringGotEscapeChars = false;
  myStringHaveEchoed = false;
  myLastStringReadTime.setToNow();
  myLogWriteStrings = false;
  //sprintf(myRawIPString, "none");
  myIPString = "";
  myBadWrite = false;
  myBadRead = false;
  myStringIgnoreReturn = false;
  myStringWrongEndChars = false;
  myErrorTracking = false;
  myFakeWrites = false;
  myDebug = false;
  resetTracking();
}

/// Normally, write() should be used instead. This is a wrapper around the sendto() system call.
AREXPORT int ArSocket::sendTo(const void *msg, size_t len)
{
#ifdef WIN32
  const int ret = ::sendto(myFD, (char*)msg, (int)len, 0, (struct sockaddr*)&mySin, (int)sizeof(mySin));
#else
  const int ret = (int) ::sendto(myFD, msg, len, 0, (struct sockaddr*)&mySin, sizeof(mySin));
#endif

  if (ret > 0)
  {
    mySends++;
    myBytesSent += ret;
  }
  return ret;
}

/// Normally, write() should be used instead. This is a wrapper around the sendto() system call.
AREXPORT int ArSocket::sendTo(const void *msg, size_t len, struct sockaddr_in *sin)
{ 
#ifdef WIN32
  const int ret = ::sendto(myFD, (char*)msg, (int)len, 0, (struct sockaddr*)sin, (int)sizeof(sockaddr_in));
#else
  const int ret = (int) ::sendto(myFD, msg, len, 0, (struct sockaddr*)sin, sizeof(struct sockaddr_in));
#endif

  if (ret > 0)
  {
    mySends++;
    myBytesSent += ret;
  }
  return ret;
}


/// Normally, read() should be used instead. This is a wrapper around the recvfrom() system call.
AREXPORT int ArSocket::recvFrom(void *msg, size_t len, sockaddr_in *sin)
{
#ifdef WIN32
  int i=sizeof(sockaddr_in);
  const int ret = ::recvfrom(myFD, (char *)msg, (int)len, 0, (struct sockaddr *)sin, &i);
#else
  socklen_t i=sizeof(sockaddr_in);
  const int ret = (int)::recvfrom(myFD, msg, len, 0, (struct sockaddr *)sin, &i);
#endif
 
  if (ret > 0)
  {
    myRecvs++;
    myBytesRecvd += ret;
  }
  return ret;
}

/**
   @param buff buffer to write from
   @param len how many bytes to write
   @return number of bytes written
**/
AREXPORT int ArSocket::write(const void *buff, size_t len)
{
  // this is for when we're faking ArNetworking commands over the text server
  if (myFakeWrites)
    return (int)len;

  if (myFD < 0)
  {
    ArLog::log(ArLog::Terse, "ArSocket::write: called after socket closed");
    return 0;
  }

  struct timeval tval;
  fd_set fdSet;
  tval.tv_sec = 0;
  tval.tv_usec = 0;
  FD_ZERO(&fdSet);
  FD_SET(myFD, &fdSet);

#ifdef WIN32
  if (select(0, NULL, &fdSet, NULL, &tval) <= 0) // fd count is ignored on windows (fd_set is an array)
#else
  if (select(myFD + 1, NULL, &fdSet, NULL, &tval) <= 0)
#endif
    return 0;
 
  int ret;
#ifdef WIN32
  ret = ::send(myFD, (char*)buff, len, 0);
#else
  ret = (int) ::write(myFD, buff, len);
#endif

  if (ret > 0)
  {
    mySends++;
    myBytesSent += ret;
  }
  if (myErrorTracking && ret < 0)
  {
    if (myNonBlocking)
    {
#ifdef WIN32
      if (WSAGetLastError() != WSAEWOULDBLOCK)
	myBadWrite = true;
#endif
#ifndef _WIN32
      if (errno != EAGAIN)
	myBadWrite = true;
#endif
    }
    else
      myBadWrite = true;
  }


  return ret;
}

/**
   @param buff buffer to read into
   @param len how many bytes to read
   @param msWait if 0, don't block, if > 0 wait this long for data
   @return number of bytes read
*/
AREXPORT int ArSocket::read(void *buff, size_t len, unsigned int msWait)
{
  if (myFD < 0)
  {
    ArLog::log(ArLog::Terse, "ArSocket::read: called after socket closed");
    return 0;
  }

  int ret;
  if (msWait != 0)
  {
    struct timeval tval;
    fd_set fdSet;
    tval.tv_sec = msWait / 1000;
    tval.tv_usec = (msWait % 1000) * 1000;
    FD_ZERO(&fdSet);
    FD_SET(myFD, &fdSet);

#ifdef WIN32
	if (select(0, &fdSet, NULL, NULL, &tval) <= 0)
	  return 0;
#else
    if (select(myFD + 1, &fdSet, NULL, NULL, &tval) <= 0)
      return 0;
#endif
  }
  ret = (int) ::recv(myFD, (char*)buff, len, 0);
  if (ret > 0)
  {
    myRecvs++;
    myBytesRecvd += ret;
  }
  if (myErrorTracking && ret < 0)
  {
    if (myNonBlocking)
    {
#ifdef WIN32
      if (WSAGetLastError() != WSAEWOULDBLOCK)
	myBadRead = true;
#endif
#ifndef _WIN32
      if (errno != EAGAIN)
	myBadRead = true;
#endif
    }
    else
      myBadRead = true;
  }

  return ret;
}


#ifndef SWIG
/*
   This cannot write more than 512 number of bytes
   @param str the string to write to the socket
   @return number of bytes written
**/
AREXPORT int ArSocket::writeString(const char *str, ...)
{
  char buf[10000];
  myWriteStringMutex.lock();
  va_list ptr;
  va_start(ptr, str);
  vsnprintf(buf, sizeof(buf) - 3, str, ptr);
  va_end(ptr);
  size_t len = strlen(buf);
  if (myStringWrongEndChars)
  {
    buf[len++] = '\n';
    buf[len++] = '\r';
  }
  else
  {
    buf[len++] = '\r';
    buf[len++] = '\n';
  }
  const int ret = write(buf, len);
  // this is after the write since we don't send NULLs out the write,
  // but we need them on the log messages or it'll crash
  buf[len++] = '\0';
  if (ret <= 0)
  {
    if (ret < 0)
      ArLog::log(ArLog::Normal, "Problem sending (ret %d errno %d) to %s: %s",
		 ret, errno, getIPString(), buf);
    else 
      ArLog::log(ArLog::Normal, "Problem sending (backed up) to %s: %s",
		 getIPString(), buf);
  }
  else if (myLogWriteStrings)
    ArLog::log(ArLog::Normal, "Sent to %s: %s", getIPString(), buf);

  myWriteStringMutex.unlock();
  return ret;
}
#endif

void ArSocket::setIPString(struct in_addr *addr)
{
  if(addr == NULL)
    return;
  unsigned char *bytes = (unsigned char *)addr;
  char s[16];
  snprintf(s, 16, "%d.%d.%d.%d", 
	    bytes[0], bytes[1], bytes[2], bytes[3]);
  s[15] = '\0';
  myIPString = s;
}


  /**
     @note This function can only read strings less than 512 characters
     long as it reads the characters into its own internal buffer (to
     compensate for some of the things the DOS telnet does).

     @param msWait if 0, don't block, if > 0 wait this long for data

     @return Data read, or an empty string (first character will be '\\0') 
       if no data was read.  If there was an error reading from the socket,
       NULL is returned.
  **/

AREXPORT char *ArSocket::readString(unsigned int msWait)
{
  size_t i;
  int n;

  bool printing = false;

  myReadStringMutex.lock();
  myStringBufEmpty[0] = '\0';

  // read one byte at a time
  for (i = myStringPos; i < sizeof(myStringBuf); i++)
  {
    n = read(&myStringBuf[i], 1, msWait);
    if (n > 0)
    {
      if (i == 0 && myStringBuf[i] < 0)
      {
	myStringGotEscapeChars = true;
      }
      if (myStringIgnoreReturn && myStringBuf[i] == '\r')
      {
	i--;
	continue;
      }

      if (myStringBuf[i] == '\n' || myStringBuf[i] == '\r')
      {
	// if we aren't at the start, it's a complete string
	if (i != 0)
	{
	  myStringGotComplete = true;
	}
	// if it is at the start, we should read basically ignore this
	// character since otherwise when we get a \n\r we're
	// returning an empty string (which is what is returned when
	// there is nothing to read, so causes problems)... so here
	// it's just calling itself and returning that since it
	// changes the logic the least
	else 
	{
	  myLastStringReadTime.setToNow();
	  if (printing)
	    ArLog::log(ArLog::Normal, 
		       "ArSocket::ReadString: calling readstring again since got \\n or \\r as the first char",
		       myStringBuf, strlen(myStringBuf));
	  myReadStringMutex.unlock();
	  return readString(msWait);
	}
	myStringBuf[i] = '\0';
	myStringPos = 0;
	myStringPosLast = 0;
	// if we have leading escape characters get rid of them
	if (myStringBuf[0] < 0)
	{
	  int ei;
	  myStringGotEscapeChars = true;
	  // increment out the escape chars
	  for (ei = 0; 
	       myStringBuf[ei] < 0 || (ei > 0 && myStringBuf[ei - 1] < 0); 
	       ei++);
	  // okay now return the good stuff
	  doStringEcho();
	  myLastStringReadTime.setToNow();
	  if (printing)
	    ArLog::log(ArLog::Normal, 
		       "ArSocket::ReadString: '%s' (%d) (got \\n or \\r)",
		       &myStringBuf[ei], strlen(&myStringBuf[ei]));
	  myReadStringMutex.unlock();
	  return &myStringBuf[ei];
	}
	// if we don't return what we got
	doStringEcho();
	myLastStringReadTime.setToNow();
	if (printing)
	  ArLog::log(ArLog::Normal, 
		     "ArSocket::ReadString: '%s' (%d) (got \\n or \\r)",
		     myStringBuf, strlen(myStringBuf));
	myReadStringMutex.unlock();
	return myStringBuf;
      }
      // if its not an ending character but was good keep going
      else
	continue;
    }
    // failed
    else if (n == 0)
    {
      myStringPos = i;
      myStringBuf[myStringPos] = '\0';
      if (printing)
	ArLog::log(ArLog::Normal, "ArSocket::ReadString: NULL (0) (got 0 bytes, means connection closed");
      myReadStringMutex.unlock();
      return NULL;
    }
    else // which means (n < 0)
    {
#ifdef WIN32
      if (WSAGetLastError() == WSAEWOULDBLOCK)
      {
	myStringPos = i;
	doStringEcho();
	if (printing)
	  ArLog::log(ArLog::Normal, "ArSocket::ReadString: '%s' (%d) (got WSAEWOULDBLOCK)",
		     myStringBufEmpty, strlen(myStringBufEmpty));
	myReadStringMutex.unlock();
	return myStringBufEmpty;
      }
#endif
#ifndef _WIN32
      if (errno == EAGAIN)
      {
	myStringPos = i;
	doStringEcho();
	if (printing)
	  ArLog::log(ArLog::Normal, 
		     "ArSocket::ReadString: '%s' (%d) (got EAGAIN)",
		     myStringBufEmpty, strlen(myStringBufEmpty));
	myReadStringMutex.unlock();
	return myStringBufEmpty;
      }
#endif
      ArLog::logErrorFromOS(ArLog::Normal, "ArSocket::readString: Error in reading from network");
      if (printing)
	ArLog::log(ArLog::Normal, "ArSocket::ReadString: NULL (0) (got 0 bytes,  error reading network)");
      myReadStringMutex.unlock();
      return NULL;
    }
  }
  // if they want a 0 length string
  ArLog::log(ArLog::Normal, "Some trouble in ArSocket::readString to %s (cannot fit string into buffer?)", getIPString());
  writeString("String too long");
      if (printing)
	ArLog::log(ArLog::Normal, "ArSocket::ReadString: NULL (0) (string too long?)");

  myReadStringMutex.unlock();
  return NULL;
}

AREXPORT void ArSocket::clearPartialReadString()
{
  myReadStringMutex.lock();
  myStringBuf[0] = '\0';
  myStringPos = 0;
  myReadStringMutex.unlock();
}

AREXPORT int ArSocket::comparePartialReadString(const char *partialString)
{
  int ret;
  myReadStringMutex.lock();
  ret = strncmp(partialString, myStringBuf, strlen(partialString));
  myReadStringMutex.unlock();
  return ret;
}

void ArSocket::doStringEcho()
{
  if (!myStringAutoEcho && !myStringEcho)
    return;

  // if we're echoing complete thel ines
  if (myStringHaveEchoed && myStringGotComplete)
  {
    write("\n\r", 2);
    myStringGotComplete = false;
  }

  // if there's nothing to send we don't need to send it
  if (myStringPosLast == myStringPos)
    return;
  
  // we probably don't need it if its doing escape chars
  if (myStringAutoEcho && myStringGotEscapeChars)
    return;

  myStringHaveEchoed = true;
  //size_t to = strchr(myStringBuf, '\0') - myStringBuf;
  write(&myStringBuf[myStringPosLast], myStringPos - myStringPosLast);
  myStringPosLast = myStringPos;
}

void ArSocket::separateHost(const char *rawHost, int rawPort, char *useHost, 
			    size_t useHostSize, int *port, bool *ok)
{
  if (useHost == NULL)
  {
    ArLog::log(ArLog::Normal, "ArSocket: useHost was NULL");
    if(ok) *ok = false;
    return;
  }
  if (port == NULL)
  {
    ArLog::log(ArLog::Normal, "ArSocket: port was NULL");
    if(ok) *ok = false;
    return;
  }

  useHost[0] = '\0';

  if (rawHost == NULL || rawHost[0] == '\0')
  {
    ArLog::log(ArLog::Normal, "ArSocket: rawHost was NULL or empty");
    if(ok) *ok = false;
    return;
  }
  
  ArArgumentBuilder separator(512, ':');
  separator.add(rawHost);

  if (separator.getArgc() <= 0)
  {
    ArLog::log(ArLog::Normal, "ArSocket: rawHost was empty");
    if(ok) *ok = false;
    return;
  }
  if (separator.getArgc() == 1)
  {
    strncpy(useHost, separator.getArg(0), useHostSize - 1);
    *port = rawPort;
    return;
  }
  if (separator.getArgc() == 2)
  {
    if (separator.isArgInt(1))
    {
      strncpy(useHost, separator.getArg(0), useHostSize - 1);
      *port = separator.getArgInt(1);
      if(ok) *ok = true;
      return;
    }
    else
    {
      ArLog::log(ArLog::Normal, "ArSocket: invalid port value (%s) given with hostname", separator.getArg(1));
      if(ok) *ok = false;
      return;
    }
  }

  // if we get down here there's too many args
  ArLog::log(ArLog::Normal, "ArSocket: too many arguments in hostname %s", separator.getFullString());
      if(ok) *ok = false;
  return;
}


AREXPORT void ArSocket::inToA(struct in_addr *addr, char *buff)
{
  strcpy(buff, inet_ntoa(*addr));
}

/// Convert @a addr into string numerical address
AREXPORT void ArSocket::inToA(struct in_addr *addr, char *buff, size_t buff_size)
{
  strncpy(buff, inet_ntoa(*addr), buff_size);
}

AREXPORT std::string ArSocket::inToA(struct in_addr& addr)
{
  return inet_ntoa(addr);
}

  
bool ArSocket::addrHost(struct in_addr &addr, char *host)
{
  struct hostent *hp;

  hp=gethostbyaddr((char*)&addr.s_addr, sizeof(addr.s_addr), AF_INET);
  if (hp)
    strcpy(host, hp->h_name); // this version of addrHost() is deprecated because we don't know size of 'host'
  else
    strcpy(host, inet_ntoa(addr)); // this version of addrHost() is deprecated because we don't know size of 'host'

  return(true);
}

AREXPORT bool ArSocket::addrHost(struct in_addr &addr, char *host, size_t len)
{
  struct hostent *hp = gethostbyaddr((char*)&addr.s_addr, sizeof(addr.s_addr), AF_INET);
  if (hp)
    strncpy(host, hp->h_name, len);
  else
    strncpy(host, inet_ntoa(addr), len);

  return(true);
}

AREXPORT std::string ArSocket::addrHost(struct in_addr &addr)
{
  struct hostent *hp = gethostbyaddr((char*)&addr.s_addr, sizeof(addr.s_addr), AF_INET);
  if (hp)
    return hp->h_name;
  else
    return inet_ntoa(addr);
}

AREXPORT std::string ArSocket::getHostName()
{
  char localhost[MaxHostNameLen];

  if (gethostname(localhost, sizeof(localhost)) == 1)
    return("");
  else
    return(localhost);
}
