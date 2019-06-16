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

#include "UDPForwarder.h"

#if _CRABNET_SUPPORT_UDPForwarder == 1

#include "GetTime.h"
#include "MTUSize.h"
#include "SocketLayer.h"
#include "RakSleep.h"
#include "DS_OrderedList.h"
#include "../Utils/LinuxStrings.h"
#include "../Utils/SocketDefines.h"
#include "errno.h"

#ifdef _WIN32
#include "../WSAStartupSingleton.h"
#else
#include <netdb.h>
#endif

#ifndef INVALID_SOCKET
#define INVALID_SOCKET -1
#endif

using namespace RakNet;
static const unsigned short DEFAULT_MAX_FORWARD_ENTRIES = 64;

namespace RakNet
{
    RAK_THREAD_DECLARATION(UpdateUDPForwarderGlobal);
}

UDPForwarder::ForwardEntry::ForwardEntry()
{
    timeoutOnNoDataMS = 0;
    socketFamily = 0;
    socket = INVALID_SOCKET;
    timeLastDatagramForwarded = RakNet::GetTimeMS();
    addr1Confirmed = UNASSIGNED_SYSTEM_ADDRESS;
    addr2Confirmed = UNASSIGNED_SYSTEM_ADDRESS;
}
UDPForwarder::ForwardEntry::~ForwardEntry()
{
    if (socket != INVALID_SOCKET)
        closesocket__(socket);
}

UDPForwarder::UDPForwarder()
{
#ifdef _WIN32
    WSAStartupSingleton::AddRef();
#endif

    isRunning = 0;
    threadRunning = 0;
    maxForwardEntries = DEFAULT_MAX_FORWARD_ENTRIES;
    nextInputId = 0;
    startForwardingInput.SetPageSize(sizeof(StartForwardingInputStruct) * 16);
    stopForwardingCommands.SetPageSize(sizeof(StopForwardingStruct) * 16);
}
UDPForwarder::~UDPForwarder()
{
    Shutdown();

#ifdef _WIN32
    WSAStartupSingleton::Deref();
#endif
}

void UDPForwarder::Startup()
{
    if (isRunning > 0)
        return;

    isRunning++;

    int errorCode = RakNet::RakThread::Create(UpdateUDPForwarderGlobal, this);

    if (errorCode != 0)
    {
        RakAssert(0);
        return;
    }

    while (threadRunning == 0)
        RakSleep(30);
}
void UDPForwarder::Shutdown()
{
    if (isRunning == 0)
        return;
    isRunning--;

    while (threadRunning > 0)
        RakSleep(30);

    for (unsigned j = 0; j < forwardListNotUpdated.Size(); j++)
        delete forwardListNotUpdated[j];
    forwardListNotUpdated.Clear(false);
}
void UDPForwarder::SetMaxForwardEntries(unsigned short maxEntries)
{
    RakAssert(maxEntries > 0 && maxEntries < 65535 / 2);
    maxForwardEntries = maxEntries;
}
int UDPForwarder::GetMaxForwardEntries() const
{
    return maxForwardEntries;
}
int UDPForwarder::GetUsedForwardEntries() const
{
    return (int) forwardListNotUpdated.Size();
}
UDPForwarderResult UDPForwarder::StartForwarding(SystemAddress source,
                                                 SystemAddress destination,
                                                 RakNet::TimeMS timeoutOnNoDataMS,
                                                 const char *forceHostAddress,
                                                 unsigned short socketFamily,
                                                 unsigned short *forwardingPort,
                                                 __UDPSOCKET__ *forwardingSocket)
{
    // Invalid parameters?
    if (timeoutOnNoDataMS == 0 || timeoutOnNoDataMS > UDP_FORWARDER_MAXIMUM_TIMEOUT
        || source == UNASSIGNED_SYSTEM_ADDRESS || destination == UNASSIGNED_SYSTEM_ADDRESS)
        return UDPFORWARDER_INVALID_PARAMETERS;

    if (isRunning == 0)
        return UDPFORWARDER_NOT_RUNNING;

    unsigned int inputId = nextInputId++;

    StartForwardingInputStruct *sfis = startForwardingInput.Allocate();
    sfis->source = source;
    sfis->destination = destination;
    sfis->timeoutOnNoDataMS = timeoutOnNoDataMS;
    RakAssert(timeoutOnNoDataMS != 0);
    if (forceHostAddress != nullptr && forceHostAddress[0] != 0)
        sfis->forceHostAddress = forceHostAddress;
    sfis->socketFamily = socketFamily;
    sfis->inputId = inputId;
    startForwardingInput.Push(sfis);

#ifdef _MSC_VER
#pragma warning( disable : 4127 ) // warning C4127: conditional expression is constant
#endif
    while (true)
    {
        RakSleep(0);
        startForwardingOutputMutex.Lock();
        for (unsigned int i = 0; i < startForwardingOutput.Size(); i++)
        {
            if (startForwardingOutput[i].inputId == inputId)
            {
                if (startForwardingOutput[i].result == UDPFORWARDER_SUCCESS)
                {
                    if (forwardingPort != nullptr)
                        *forwardingPort = startForwardingOutput[i].forwardingPort;
                    if (forwardingSocket != nullptr)
                        *forwardingSocket = startForwardingOutput[i].forwardingSocket;
                }
                UDPForwarderResult res = startForwardingOutput[i].result;
                startForwardingOutput.RemoveAtIndex(i);
                startForwardingOutputMutex.Unlock();
                return res;
            }
        }
        startForwardingOutputMutex.Unlock();
    }
}
void UDPForwarder::StopForwarding(SystemAddress source, SystemAddress destination)
{
    StopForwardingStruct *sfs = stopForwardingCommands.Allocate();
    sfs->destination = destination;
    sfs->source = source;
    stopForwardingCommands.Push(sfs);
}
void UDPForwarder::RecvFrom(RakNet::TimeMS curTime, ForwardEntry *forwardEntry)
{
#ifndef __native_client__
    char data[MAXIMUM_MTU_SIZE];

#if CRABNET_SUPPORT_IPV6 == 1
    sockaddr_storage their_addr {0};
    socklen_t sockLen;
    socklen_t *socketlenPtr = &sockLen;
    sockaddr_in *sockAddrIn;
    sockaddr_in6 *sockAddrIn6;
    sockLen = sizeof(their_addr);
    sockaddr *sockAddrPtr = (sockaddr *) &their_addr;
#else
    sockaddr_in sockAddrIn {0};
    socklen_t len2 = sizeof(sockAddrIn);
    sockAddrIn.sin_family = AF_INET;
#endif

#if defined(__GNUC__)
    #if defined(MSG_DONTWAIT)
    const int flag = MSG_DONTWAIT;
    #else
    const int flag = 0x40;
    #endif
#else
    const int flag = 0;
#endif

    int receivedDataLen, len = 0;
    //unsigned short portnum=0;

#if CRABNET_SUPPORT_IPV6 == 1
    receivedDataLen = recvfrom__(forwardEntry->socket, data, MAXIMUM_MTU_SIZE, flag, sockAddrPtr, socketlenPtr);
#else
    receivedDataLen = recvfrom__( forwardEntry->socket, data, MAXIMUM_MTU_SIZE, flag, ( sockaddr* ) & sockAddrIn, ( socklen_t* ) & len2 );
#endif

    if (receivedDataLen < 0)
    {
#if defined(_WIN32) && defined(_DEBUG)
        DWORD dwIOError = WSAGetLastError();

        if (dwIOError != WSAECONNRESET && dwIOError != WSAEINTR && dwIOError != WSAETIMEDOUT && dwIOError != WSAEWOULDBLOCK)
        {
            LPVOID messageBuffer;
            FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
                ( LPTSTR ) & messageBuffer, 0, NULL );
            // something has gone wrong here...
            CRABNET_DEBUG_PRINTF( "recvfrom failed:Error code - %d\n%s", dwIOError, messageBuffer );

            //Free the buffer.
            LocalFree( messageBuffer );
        }
#else
        if (errno != EAGAIN && errno != 0
#if defined(__GNUC__)
            && errno != EWOULDBLOCK
#endif
            )
        {
            printf("errno=%i\n", errno);
        }
#endif
    }

    if (receivedDataLen <= 0)
        return;

    SystemAddress receivedAddr;
#if CRABNET_SUPPORT_IPV6 == 1
    if (their_addr.ss_family == AF_INET)
    {
        sockAddrIn = (sockaddr_in *) &their_addr;
        sockAddrIn6 = 0;
        memcpy(&receivedAddr.address.addr4, sockAddrIn, sizeof(sockaddr_in));
    }
    else
    {
        sockAddrIn = 0;
        sockAddrIn6 = (sockaddr_in6 *) &their_addr;
        memcpy(&receivedAddr.address.addr6, sockAddrIn6, sizeof(sockaddr_in6));
    }
#else
    memcpy(&receivedAddr.address.addr4,&sockAddrIn,sizeof(sockaddr_in));
#endif
    //portnum=receivedAddr.GetPort();

    SystemAddress forwardTarget;

    bool confirmed1 = forwardEntry->addr1Confirmed != UNASSIGNED_SYSTEM_ADDRESS;
    bool confirmed2 = forwardEntry->addr2Confirmed != UNASSIGNED_SYSTEM_ADDRESS;
    bool matchConfirmed1 = confirmed1 && forwardEntry->addr1Confirmed == receivedAddr;
    bool matchConfirmed2 = confirmed2 && forwardEntry->addr2Confirmed == receivedAddr;
    bool matchUnconfirmed1 = forwardEntry->addr1Unconfirmed.EqualsExcludingPort(receivedAddr);
    bool matchUnconfirmed2 = forwardEntry->addr2Unconfirmed.EqualsExcludingPort(receivedAddr);

    if (matchConfirmed1 == true || (matchConfirmed2 == false && confirmed1 == false && matchUnconfirmed1 == true))
    {
        // Forward to addr2
        if (forwardEntry->addr1Confirmed == UNASSIGNED_SYSTEM_ADDRESS)
            forwardEntry->addr1Confirmed = receivedAddr;
        if (forwardEntry->addr2Confirmed != UNASSIGNED_SYSTEM_ADDRESS)
            forwardTarget = forwardEntry->addr2Confirmed;
        else
            forwardTarget = forwardEntry->addr2Unconfirmed;
    }
    else if (matchConfirmed2 || (!confirmed2 && matchUnconfirmed2))
    {
        // Forward to addr1
        if (forwardEntry->addr2Confirmed == UNASSIGNED_SYSTEM_ADDRESS)
            forwardEntry->addr2Confirmed = receivedAddr;
        if (forwardEntry->addr1Confirmed != UNASSIGNED_SYSTEM_ADDRESS)
            forwardTarget = forwardEntry->addr1Confirmed;
        else
            forwardTarget = forwardEntry->addr1Unconfirmed;
    }
    else
        return;

    // Forward to dest
//     sockaddr_in saOut;
//     saOut.sin_port = forwardTarget.GetPortNetworkOrder(); // User port
//     saOut.sin_addr.s_addr = forwardTarget.address.addr4.sin_addr.s_addr;
//     saOut.sin_family = AF_INET;
    if (forwardTarget.address.addr4.sin_family == AF_INET)
    {
        do
        {
            len = sendto__(forwardEntry->socket,
                           data,
                           receivedDataLen,
                           0,
                           (const sockaddr *) &forwardTarget.address.addr4,
                           sizeof(sockaddr_in));
        }
        while (len == 0);
    }
#if CRABNET_SUPPORT_IPV6 == 1
    else
    {
        do
        {
            len = sendto__(forwardEntry->socket,
                           data,
                           receivedDataLen,
                           0,
                           (const sockaddr *) &forwardTarget.address.addr6,
                           sizeof(sockaddr_in6));
        }
        while (len == 0);
    }
#endif

    forwardEntry->timeLastDatagramForwarded = curTime;
#endif  // __native_client__
}
void UDPForwarder::UpdateUDPForwarder()
{
    /*
#if !defined(SN_TARGET_PSP2)
    timeval tv;
    tv.tv_sec=0;
    tv.tv_usec=0;
#endif
    */

    RakNet::TimeMS curTime = RakNet::GetTimeMS();

    StartForwardingInputStruct *sfis;
    StartForwardingOutputStruct sfos;
    sfos.forwardingSocket = INVALID_SOCKET;
    sfos.forwardingPort = 0;
    sfos.inputId = 0;
    sfos.result = UDPFORWARDER_RESULT_COUNT;

#ifdef _MSC_VER
#pragma warning( disable : 4127 ) // warning C4127: conditional expression is constant
#endif
    while (true)
    {
        sfis = startForwardingInput.Pop();
        if (sfis == nullptr)
            break;

        if (GetUsedForwardEntries() > maxForwardEntries)
            sfos.result = UDPFORWARDER_NO_SOCKETS;
        else
        {
            sfos.result = UDPFORWARDER_RESULT_COUNT;

            for (unsigned i = 0; i < forwardListNotUpdated.Size(); i++)
            {
                if ((forwardListNotUpdated[i]->addr1Unconfirmed == sfis->source &&
                    forwardListNotUpdated[i]->addr2Unconfirmed == sfis->destination)
                    || (forwardListNotUpdated[i]->addr1Unconfirmed == sfis->destination &&
                        forwardListNotUpdated[i]->addr2Unconfirmed == sfis->source))
                {
                    ForwardEntry *fe = forwardListNotUpdated[i];
                    sfos.forwardingPort = SocketLayer::GetLocalPort(fe->socket);
                    sfos.forwardingSocket = fe->socket;
                    sfos.result = UDPFORWARDER_FORWARDING_ALREADY_EXISTS;
                    break;
                }
            }

            if (sfos.result == UDPFORWARDER_RESULT_COUNT)
            {
                int sock_opt;
                sockaddr_in listenerSocketAddress {0};
                listenerSocketAddress.sin_port = 0;
                auto fe = new ForwardEntry;
                fe->addr1Unconfirmed = sfis->source;
                fe->addr2Unconfirmed = sfis->destination;
                fe->timeoutOnNoDataMS = sfis->timeoutOnNoDataMS;

#if CRABNET_SUPPORT_IPV6 != 1
                fe->socket = socket__( AF_INET, SOCK_DGRAM, 0);
                listenerSocketAddress.sin_family = AF_INET;
                if (sfis->forceHostAddress.IsEmpty() == false)
                    listenerSocketAddress.sin_addr.s_addr = inet_addr__(sfis->forceHostAddress.C_String());
                else
                    listenerSocketAddress.sin_addr.s_addr = INADDR_ANY;
                int ret = bind__(fe->socket, (sockaddr *) & listenerSocketAddress, sizeof(listenerSocketAddress));
                if (ret==-1)
                {
                    delete fe;
                    sfos.result = UDPFORWARDER_BIND_FAILED;
                }
                else
                    sfos.result = UDPFORWARDER_SUCCESS;

#else // CRABNET_SUPPORT_IPV6==1
                addrinfo hints {0};
                hints.ai_family = sfis->socketFamily;
                hints.ai_socktype = SOCK_DGRAM;      // UDP sockets
                hints.ai_flags = AI_PASSIVE;         // fill in my IP for me
                addrinfo *servinfo = nullptr;        // will point to the results

                if (sfis->forceHostAddress.IsEmpty() || sfis->forceHostAddress == "UNASSIGNED_SYSTEM_ADDRESS")
                    getaddrinfo(nullptr, "0", &hints, &servinfo);
                else
                    getaddrinfo(sfis->forceHostAddress.C_String(), "0", &hints, &servinfo);

                for (addrinfo *aip = servinfo; aip != nullptr; aip = aip->ai_next)
                {
                    // Open socket. The address type depends on what getaddrinfo() gave us.
                    fe->socket = socket__(aip->ai_family, aip->ai_socktype, aip->ai_protocol);
                    if (fe->socket != INVALID_SOCKET)
                    {
                        int ret = bind__(fe->socket, aip->ai_addr, (int) aip->ai_addrlen);
                        if (ret >= 0)
                            break;
                        else
                        {
                            closesocket__(fe->socket);
                            fe->socket = INVALID_SOCKET;
                        }
                    }
                }

                if (fe->socket == INVALID_SOCKET)
                    sfos.result = UDPFORWARDER_BIND_FAILED;
                else
                    sfos.result = UDPFORWARDER_SUCCESS;
#endif  // CRABNET_SUPPORT_IPV6==1

                if (sfos.result == UDPFORWARDER_SUCCESS)
                {
                    sfos.forwardingPort = SocketLayer::GetLocalPort(fe->socket); // -V774
                    sfos.forwardingSocket = fe->socket; // -V774

                    sock_opt = 1024 * 256;
                    setsockopt__(fe->socket, SOL_SOCKET, SO_RCVBUF, (char *) &sock_opt, sizeof(sock_opt)); // -V774
                    sock_opt = 0;
                    setsockopt__(fe->socket, SOL_SOCKET, SO_LINGER, (char *) &sock_opt, sizeof(sock_opt)); // -V774
#ifdef _WIN32
                    unsigned long nonblocking = 1;
                    ioctlsocket__( fe->socket, FIONBIO, &nonblocking );
#else
                    fcntl(fe->socket, F_SETFL, O_NONBLOCK); // -V774
#endif
                    forwardListNotUpdated.Insert(fe); // -V774
                }
            }
        }

        // Push result
        sfos.inputId = sfis->inputId;
        startForwardingOutputMutex.Lock();
        startForwardingOutput.Push(sfos);
        startForwardingOutputMutex.Unlock();

        startForwardingInput.Deallocate(sfis);
    }
    StopForwardingStruct *sfs;

#ifdef _MSC_VER
#pragma warning( disable : 4127 ) // warning C4127: conditional expression is constant
#endif
    while (true)
    {
        sfs = stopForwardingCommands.Pop();
        if (sfs == nullptr)
            break;

        ForwardEntry *fe;
        for (unsigned int i = 0; i < forwardListNotUpdated.Size(); i++)
        {
            if ((forwardListNotUpdated[i]->addr1Unconfirmed == sfs->source &&
                forwardListNotUpdated[i]->addr2Unconfirmed == sfs->destination)
                || (forwardListNotUpdated[i]->addr1Unconfirmed == sfs->destination &&
                    forwardListNotUpdated[i]->addr2Unconfirmed == sfs->source))
            {
                fe = forwardListNotUpdated[i];
                forwardListNotUpdated.RemoveAtIndexFast(i);
                delete fe;
                break;
            }
        }

        stopForwardingCommands.Deallocate(sfs);
    }

    unsigned int i = 0;
    while (i < forwardListNotUpdated.Size())
    {
        if (curTime > forwardListNotUpdated[i]->timeLastDatagramForwarded && // Account for timestamp wrap
            curTime > forwardListNotUpdated[i]->timeLastDatagramForwarded + forwardListNotUpdated[i]->timeoutOnNoDataMS)
        {
            delete forwardListNotUpdated[i];
            forwardListNotUpdated.RemoveAtIndex(i);
        }
        else
            i++;
    }

    ForwardEntry *forwardEntry;
    for (i = 0; i < forwardListNotUpdated.Size(); i++)
    {
        forwardEntry = forwardListNotUpdated[i];
        RecvFrom(curTime, forwardEntry);
    }
}

namespace RakNet
{
    RAK_THREAD_DECLARATION(UpdateUDPForwarderGlobal)
    {
        auto udpForwarder = (UDPForwarder *) arguments;

        udpForwarder->threadRunning++;
        while (udpForwarder->isRunning > 0)
        {
            udpForwarder->UpdateUDPForwarder();

            // 12/1/2010 Do not change from 0
            // See http://www.jenkinssoftware.com/forum/index.php?topic=4033.0;topicseen
            // Avoid 100% reported CPU usage
            if (udpForwarder->forwardListNotUpdated.Size() == 0)
                RakSleep(30);
            else
                RakSleep(0);
        }
        udpForwarder->threadRunning--;

#if (defined(_WIN32_WCE) || defined(_WIN32))
        return 0;
#else
        return nullptr;
#endif
    }
} // namespace RakNet

#endif // #if _CRABNET_SUPPORT_FileOperations==1
