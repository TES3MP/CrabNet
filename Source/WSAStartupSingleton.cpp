/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  Copyright (c) 2016-2018, TES3MP Team
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant 
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#ifdef _WIN32
#include "WSAStartupSingleton.h"


#include <winsock2.h>
#include <ws2tcpip.h>

#include "RakNetDefines.h"
#include <stdio.h>

int WSAStartupSingleton::refCount=0;

WSAStartupSingleton::WSAStartupSingleton() {}
WSAStartupSingleton::~WSAStartupSingleton() {}
void WSAStartupSingleton::AddRef(void)
{
#if defined(_WIN32)

    refCount++;

    if (refCount!=1)
        return;
    WSADATA winsockInfo;
    if ( WSAStartup( MAKEWORD( 2, 2 ), &winsockInfo ) != 0 )
    {
#if  defined(_DEBUG)
        DWORD dwIOError = GetLastError();
        LPVOID messageBuffer;
        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
            ( LPTSTR ) & messageBuffer, 0, NULL );
        // something has gone wrong here...
        RAKNET_DEBUG_PRINTF( "WSAStartup failed:Error code - %d\n%s", dwIOError, messageBuffer );
        //Free the buffer.
        LocalFree( messageBuffer );
#endif
    }

#endif
}
void WSAStartupSingleton::Deref(void)
{
#if defined(_WIN32)
    if (refCount==0)
        return;

    if (refCount>1)
    {
        refCount--;
        return;
    }

    WSACleanup();

    refCount=0;
#endif
}
#endif