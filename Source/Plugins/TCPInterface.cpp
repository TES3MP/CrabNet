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

#include "NativeFeatureIncludes.h"

#if _CRABNET_SUPPORT_TCPInterface == 1

/// \file
/// \brief A simple TCP based server allowing sends and receives.  Can be connected to by a telnet client.
///



#include "TCPInterface.h"
#ifdef _WIN32
typedef int socklen_t;
#include "../WSAStartupSingleton.h"
#else
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#endif

#include <string.h>
#include "RakAssert.h"
#include <stdio.h>
#include "RakAssert.h"
#include "RakSleep.h"
#include "StringCompressor.h"
#include "StringTable.h"
#include "Itoa.h"
#include "SocketLayer.h"
#include "../Utils/SocketDefines.h"

#if (defined(__GNUC__) || defined(__GCCXML__)) && !defined(__WIN32__)
#include <netdb.h>
#endif

#ifdef _DO_PRINTF
#endif

namespace RakNet
{
    RAK_THREAD_DECLARATION(UpdateTCPInterfaceLoop);
    RAK_THREAD_DECLARATION(ConnectionAttemptLoop);
}
#ifdef _MSC_VER
#pragma warning( push )
#endif

using namespace RakNet;

STATIC_FACTORY_DEFINITIONS(TCPInterface, TCPInterface)

TCPInterface::TCPInterface()
{
    threadPriority = 0;
    isStarted = 0;
    threadRunning = 0;
    listenSocket = 0;
    remoteClients = nullptr;
    remoteClientsLength = 0;

#if OPEN_SSL_CLIENT_SUPPORT == 1
    ctx = 0;
    meth = 0;
#endif

#ifdef _WIN32
    WSAStartupSingleton::AddRef();
#endif
}

TCPInterface::~TCPInterface()
{
    Stop();
#ifdef _WIN32
    WSAStartupSingleton::Deref();
#endif

    delete[] remoteClients;

}

bool TCPInterface::CreateListenSocket(unsigned short port, unsigned short maxIncomingConnections,
                                      unsigned short socketFamily, const char *bindAddress)
{
#if CRABNET_SUPPORT_IPV6 != 1
    (void) socketFamily;
    listenSocket = socket__(AF_INET, SOCK_STREAM, 0);
    if ((int) listenSocket == -1)
        return false;

    struct sockaddr_in serverAddress;
    memset(&serverAddress, 0, sizeof(sockaddr_in));
    serverAddress.sin_family = AF_INET;
    if (bindAddress && bindAddress[0])
        serverAddress.sin_addr.s_addr = inet_addr__(bindAddress);
    else
        serverAddress.sin_addr.s_addr = INADDR_ANY;

    serverAddress.sin_port = htons(port);

    SocketLayer::SetSocketOptions(listenSocket, false, false);

    if (bind__(listenSocket, (struct sockaddr *) &serverAddress, sizeof(serverAddress)) < 0)
        return false;

    listen__(listenSocket, maxIncomingConnections);
#else
    struct addrinfo hints;
    memset(&hints, 0, sizeof (addrinfo)); // make sure the struct is empty
    hints.ai_family = socketFamily;     // don't care IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP sockets
    hints.ai_flags = AI_PASSIVE;     // fill in my IP for me
    struct addrinfo *servinfo = 0, *aip;  // will point to the results
    char portStr[32];
    Itoa(port,portStr,10);

    getaddrinfo(0, portStr, &hints, &servinfo);
    for (aip = servinfo; aip != NULL; aip = aip->ai_next)
    {
        // Open socket. The address type depends on what
        // getaddrinfo() gave us.
        listenSocket = socket__(aip->ai_family, aip->ai_socktype, aip->ai_protocol);
        if (listenSocket != 0)
        {
            if (bind__(listenSocket, aip->ai_addr, (int) aip->ai_addrlen) >= 0)
                break;
            else
            {
                closesocket__(listenSocket);
                listenSocket = 0;
            }
        }
    }

    if (listenSocket == 0)
        return false;

    SocketLayer::SetSocketOptions(listenSocket, false, false);

    listen__(listenSocket, maxIncomingConnections);
#endif // #if CRABNET_SUPPORT_IPV6!=1

    return true;
}

bool TCPInterface::Start(unsigned short port, unsigned short maxIncomingConnections, unsigned short maxConnections,
                         int _threadPriority, unsigned short socketFamily, const char *bindAddress)
{
#ifdef __native_client__
    return false;
#else
    (void) socketFamily;

    if (isStarted > 0)
        return false;

    threadPriority = _threadPriority;

    if (threadPriority == -99999)
    {
#if   defined(_WIN32)
        threadPriority = 0;
#else
        threadPriority = 1000;
#endif
    }

    isStarted++;
    if (maxIncomingConnections == 0)
        maxConnections = 1;
    else
        maxConnections = maxIncomingConnections;

    remoteClientsLength = maxConnections;
    remoteClients = new RemoteClient[maxConnections];


    listenSocket = 0;
    if (maxIncomingConnections > 0)
        CreateListenSocket(port, maxIncomingConnections, socketFamily, bindAddress);


    // Start the update thread
    int errorCode = RakNet::RakThread::Create(UpdateTCPInterfaceLoop, this, threadPriority);


    if (errorCode != 0)
        return false;

    while (threadRunning == 0)
        RakSleep(0);

    for (unsigned int i = 0; i < messageHandlerList.Size(); i++)
        messageHandlerList[i]->OnRakPeerStartup();

    return true;
#endif  // __native_client__
}

void TCPInterface::Stop(void)
{
    for (unsigned int i = 0; i < messageHandlerList.Size(); i++)
        messageHandlerList[i]->OnRakPeerShutdown();

#ifndef __native_client__
    if (isStarted == 0)
        return;

#if OPEN_SSL_CLIENT_SUPPORT == 1
    for (unsigned int i = 0; i < remoteClientsLength; i++)
        remoteClients[i].DisconnectSSL();
#endif

    isStarted--;

    if (listenSocket != 0)
    {
#ifdef _WIN32
        shutdown__(listenSocket, SD_BOTH);

#else
        shutdown__(listenSocket, SHUT_RDWR);
#endif
        closesocket__(listenSocket);
    }

    // Abort waiting connect calls
    blockingSocketListMutex.Lock();
    for (unsigned int i = 0; i < blockingSocketList.Size(); i++)
        closesocket__(blockingSocketList[i]);
    blockingSocketListMutex.Unlock();

    // Wait for the thread to stop
    while (threadRunning > 0)
        RakSleep(15);

    RakSleep(100);

    listenSocket = 0;

    // Stuff from here on to the end of the function is not threadsafe
    for (unsigned int i = 0; i < (unsigned int) remoteClientsLength; i++)
    {
        closesocket__(remoteClients[i].socket);
#if OPEN_SSL_CLIENT_SUPPORT == 1
        remoteClients[i].FreeSSL();
#endif
    }
    remoteClientsLength = 0;
    delete[] remoteClients;
    remoteClients = nullptr;

    incomingMessages.Clear();
    newIncomingConnections.Clear();
    newRemoteClients.Clear();
    lostConnections.Clear();
    requestedCloseConnections.Clear();
    failedConnectionAttempts.Clear();
    completedConnectionAttempts.Clear();
    failedConnectionAttempts.Clear();
    for (unsigned int i = 0; i < headPush.Size(); i++)
        DeallocatePacket(headPush[i]);
    headPush.Clear();
    for (unsigned int i = 0; i < tailPush.Size(); i++)
        DeallocatePacket(tailPush[i]);
    tailPush.Clear();

#if OPEN_SSL_CLIENT_SUPPORT == 1
    SSL_CTX_free (ctx);
    startSSL.Clear();
    activeSSLConnections.Clear(false);
#endif

#endif  // __native_client__
}

SystemAddress
TCPInterface::Connect(const char *host, unsigned short remotePort, bool block, unsigned short socketFamily,
                      const char *bindAddress)
{
    if (threadRunning == 0)
        return UNASSIGNED_SYSTEM_ADDRESS;

    int newRemoteClientIndex;
    for (newRemoteClientIndex = 0; newRemoteClientIndex < remoteClientsLength; newRemoteClientIndex++)
    {
        auto &newRemoteClient = remoteClients[newRemoteClientIndex];
        newRemoteClient.isActiveMutex.Lock();
        if (!remoteClients[newRemoteClientIndex].isActive)
        {
            newRemoteClient.SetActive(true);
            newRemoteClient.isActiveMutex.Unlock();
            break;
        }
        newRemoteClient.isActiveMutex.Unlock();
    }

    /*if (newRemoteClientIndex == -1)
        return UNASSIGNED_SYSTEM_ADDRESS;*/

    if (block)
    {
        SystemAddress systemAddress;
        systemAddress.FromString(host);
        systemAddress.SetPortHostOrder(remotePort);
        systemAddress.systemIndex = (SystemIndex) newRemoteClientIndex;
        char buffout[128];
        systemAddress.ToString(false, buffout);

        auto &newRemoteClient = remoteClients[newRemoteClientIndex];
        __TCPSOCKET__ sockfd = SocketConnect(buffout, remotePort, socketFamily, bindAddress);
        if (sockfd == 0)
        {
            newRemoteClient.isActiveMutex.Lock();
            newRemoteClient.SetActive(false);
            newRemoteClient.isActiveMutex.Unlock();

            failedConnectionAttemptMutex.Lock();
            failedConnectionAttempts.Push(systemAddress);
            failedConnectionAttemptMutex.Unlock();

            return UNASSIGNED_SYSTEM_ADDRESS;
        }

        newRemoteClient.socket = sockfd;
        newRemoteClient.systemAddress = systemAddress;

        completedConnectionAttemptMutex.Lock();
        completedConnectionAttempts.Push(newRemoteClient.systemAddress);
        completedConnectionAttemptMutex.Unlock();

        return newRemoteClient.systemAddress;
    }
    else
    {
        ThisPtrPlusSysAddr *s = new ThisPtrPlusSysAddr;
        s->systemAddress.FromStringExplicitPort(host, remotePort);
        s->systemAddress.systemIndex = (SystemIndex) newRemoteClientIndex;
        if (bindAddress)
            strcpy(s->bindAddress, bindAddress);
        else
            s->bindAddress[0] = 0;
        s->tcpInterface = this;
        s->socketFamily = socketFamily;

        // Start the connection thread
        int errorCode = RakNet::RakThread::Create(ConnectionAttemptLoop, s, threadPriority);

        if (errorCode != 0)
        {
            failedConnectionAttempts.Push(s->systemAddress);
            delete s;
        }
        return UNASSIGNED_SYSTEM_ADDRESS;
    }
}

#if OPEN_SSL_CLIENT_SUPPORT == 1
void TCPInterface::StartSSLClient(SystemAddress systemAddress)
{
    if (ctx==0)
    {
        sharedSslMutex.Lock();
        SSLeay_add_ssl_algorithms();
        meth = (SSL_METHOD*) SSLv23_client_method();
        SSL_load_error_strings();
        ctx = SSL_CTX_new (meth);
        RakAssert(ctx!=0);
        sharedSslMutex.Unlock();
    }

    SystemAddress *id = startSSL.Allocate(  );
    *id=systemAddress;
    startSSL.Push(id);
    unsigned index = activeSSLConnections.GetIndexOf(systemAddress);
    if (index==(unsigned)-1)
        activeSSLConnections.Insert(systemAddress,);
}
bool TCPInterface::IsSSLActive(SystemAddress systemAddress)
{
    return activeSSLConnections.GetIndexOf(systemAddress)!=-1;
}
#endif

void TCPInterface::Send(const char *data, unsigned length, const SystemAddress &systemAddress, bool broadcast)
{
    SendList(&data, &length, 1, systemAddress, broadcast);
}

bool TCPInterface::SendList(const char **data, const unsigned int *lengths, const int numParameters,
                            const SystemAddress &systemAddress, bool broadcast)
{
    if (isStarted == 0)
        return false;
    if (data == nullptr)
        return false;
    if (systemAddress == UNASSIGNED_SYSTEM_ADDRESS && !broadcast)
        return false;
    unsigned int totalLength = 0;
    for (int i = 0; i < numParameters; i++)
    {
        if (lengths[i] > 0)
            totalLength += lengths[i];
    }
    if (totalLength == 0)
        return false;

    if (broadcast)
    {
        // Send to all, possible exception system
        for (int i = 0; i < remoteClientsLength; i++)
        {
            if (remoteClients[i].systemAddress != systemAddress)
                remoteClients[i].SendOrBuffer(data, lengths, numParameters);
        }
    }
    else
    {
        // Send to this player
        const SystemIndex &si = systemAddress.systemIndex;
        if (si < remoteClientsLength && remoteClients[si].systemAddress == systemAddress)
            remoteClients[si].SendOrBuffer(data, lengths, numParameters);
        else
        {
            for (int i = 0; i < remoteClientsLength; i++)
            {
                if (remoteClients[i].systemAddress == systemAddress)
                    remoteClients[i].SendOrBuffer(data, lengths, numParameters);
            }
        }
    }

    return true;
}

bool TCPInterface::ReceiveHasPackets(void)
{
    return !headPush.IsEmpty() || !incomingMessages.IsEmpty() || !tailPush.IsEmpty();
}

Packet *TCPInterface::Receive(void)
{
    for (unsigned int i = 0; i < messageHandlerList.Size(); i++)
        messageHandlerList[i]->Update();

    Packet *outgoingPacket = ReceiveInt();

    if (outgoingPacket)
    {
        for (unsigned int i = 0; i < messageHandlerList.Size(); i++)
        {
            PluginReceiveResult pluginResult = messageHandlerList[i]->OnReceive(outgoingPacket);
            if (pluginResult == RR_STOP_PROCESSING_AND_DEALLOCATE)
            {
                DeallocatePacket(outgoingPacket);
                outgoingPacket = 0; // Will do the loop again and get another packet
                break; // break out of the enclosing for
            }
            else if (pluginResult == RR_STOP_PROCESSING)
            {
                outgoingPacket = 0;
                break;
            }
        }
    }
    return outgoingPacket;
}

Packet *TCPInterface::ReceiveInt(void)
{
    if (isStarted == 0)
        return 0;
    if (!headPush.IsEmpty())
        return headPush.Pop();
    Packet *p = incomingMessages.PopInaccurate();
    if (p)
        return p;
    if (!tailPush.IsEmpty())
        return tailPush.Pop();
    return 0;
}


void TCPInterface::AttachPlugin(PluginInterface2 *plugin)
{
    if (messageHandlerList.GetIndexOf(plugin) == MAX_UNSIGNED_LONG)
    {
        messageHandlerList.Insert(plugin);
        plugin->SetTCPInterface(this);
        plugin->OnAttach();
    }
}

void TCPInterface::DetachPlugin(PluginInterface2 *plugin)
{
    if (plugin == 0)
        return;

    unsigned int index = messageHandlerList.GetIndexOf(plugin);
    if (index != MAX_UNSIGNED_LONG)
    {
        messageHandlerList[index]->OnDetach();
        // Unordered list so delete from end for speed
        messageHandlerList[index] = messageHandlerList[messageHandlerList.Size() - 1];
        messageHandlerList.RemoveFromEnd();
        plugin->SetTCPInterface(0);
    }
}

void TCPInterface::CloseConnection(SystemAddress systemAddress)
{
    if (isStarted == 0)
        return;
    if (systemAddress == UNASSIGNED_SYSTEM_ADDRESS)
        return;

    for (unsigned int i = 0; i < messageHandlerList.Size(); i++)
        messageHandlerList[i]->OnClosedConnection(systemAddress, UNASSIGNED_CRABNET_GUID, LCR_CLOSED_BY_USER);

    if (systemAddress.systemIndex < remoteClientsLength &&
        remoteClients[systemAddress.systemIndex].systemAddress == systemAddress)
    {
        remoteClients[systemAddress.systemIndex].isActiveMutex.Lock();
        remoteClients[systemAddress.systemIndex].SetActive(false);
        remoteClients[systemAddress.systemIndex].isActiveMutex.Unlock();
    }
    else
    {
        for (int i = 0; i < remoteClientsLength; i++)
        {
            remoteClients[i].isActiveMutex.Lock();
            if (remoteClients[i].isActive && remoteClients[i].systemAddress == systemAddress)
            {
                remoteClients[systemAddress.systemIndex].SetActive(false);
                remoteClients[i].isActiveMutex.Unlock();
                break;
            }
            remoteClients[i].isActiveMutex.Unlock();
        }
    }


#if OPEN_SSL_CLIENT_SUPPORT == 1
    unsigned index = activeSSLConnections.GetIndexOf(systemAddress);
    if (index!=(unsigned)-1)
        activeSSLConnections.RemoveAtIndex(index);
#endif
}

void TCPInterface::DeallocatePacket(Packet *packet)
{
    if (packet == 0)
        return;
    if (packet->deleteData)
    {
        free(packet->data);
        incomingMessages.Deallocate(packet);
    }
    else
    {
        // Came from userspace AllocatePacket
        free(packet->data);
        delete packet;
    }
}

Packet *TCPInterface::AllocatePacket(unsigned dataSize)
{
    Packet *p = new Packet;
    p->data = (unsigned char *) malloc(dataSize);
    p->length = dataSize;
    p->bitSize = BYTES_TO_BITS(dataSize);
    p->deleteData = false;
    p->guid = UNASSIGNED_CRABNET_GUID;
    p->systemAddress = UNASSIGNED_SYSTEM_ADDRESS;
    p->systemAddress.systemIndex = (SystemIndex) -1;
    return p;
}

void TCPInterface::PushBackPacket(Packet *packet, bool pushAtHead)
{
    if (pushAtHead)
        headPush.Push(packet);
    else
        tailPush.Push(packet);
}

bool TCPInterface::WasStarted(void) const
{
    return threadRunning > 0;
}

SystemAddress TCPInterface::HasCompletedConnectionAttempt(void)
{
    SystemAddress sysAddr = UNASSIGNED_SYSTEM_ADDRESS;
    completedConnectionAttemptMutex.Lock();
    if (!completedConnectionAttempts.IsEmpty())
        sysAddr = completedConnectionAttempts.Pop();
    completedConnectionAttemptMutex.Unlock();

    if (sysAddr != UNASSIGNED_SYSTEM_ADDRESS)
    {
        unsigned int i;
        for (i = 0; i < messageHandlerList.Size(); i++)
            messageHandlerList[i]->OnNewConnection(sysAddr, UNASSIGNED_CRABNET_GUID, true);
    }

    return sysAddr;
}

SystemAddress TCPInterface::HasFailedConnectionAttempt(void)
{
    SystemAddress sysAddr = UNASSIGNED_SYSTEM_ADDRESS;
    failedConnectionAttemptMutex.Lock();
    if (!failedConnectionAttempts.IsEmpty())
        sysAddr = failedConnectionAttempts.Pop();
    failedConnectionAttemptMutex.Unlock();

    if (sysAddr != UNASSIGNED_SYSTEM_ADDRESS)
    {
        for (unsigned int i = 0; i < messageHandlerList.Size(); i++)
        {
            Packet p;
            p.systemAddress = sysAddr;
            p.data = 0;
            p.length = 0;
            p.bitSize = 0;
            messageHandlerList[i]->OnFailedConnectionAttempt(&p, FCAR_CONNECTION_ATTEMPT_FAILED);
        }
    }

    return sysAddr;
}

SystemAddress TCPInterface::HasNewIncomingConnection(void)
{
    SystemAddress *out = newIncomingConnections.PopInaccurate();
    if (out)
    {
        SystemAddress out2 = *out;
        newIncomingConnections.Deallocate(out);

        for (unsigned int i = 0; i < messageHandlerList.Size(); i++)
            messageHandlerList[i]->OnNewConnection(out2, UNASSIGNED_CRABNET_GUID, true);

        return *out;
    }

    return UNASSIGNED_SYSTEM_ADDRESS;
}

SystemAddress TCPInterface::HasLostConnection(void)
{
    SystemAddress *out = lostConnections.PopInaccurate();
    if (out)
    {
        SystemAddress out2 = *out;
        lostConnections.Deallocate(out);

        for (unsigned int i = 0; i < messageHandlerList.Size(); i++)
            messageHandlerList[i]->OnClosedConnection(out2, UNASSIGNED_CRABNET_GUID, LCR_DISCONNECTION_NOTIFICATION);

        return *out;
    }

    return UNASSIGNED_SYSTEM_ADDRESS;
}

void TCPInterface::GetConnectionList(SystemAddress *remoteSystems, unsigned short *numberOfSystems) const
{
    unsigned short systemCount = 0;
    unsigned short maxToWrite = *numberOfSystems;
    for (int i = 0; i < remoteClientsLength; i++)
    {
        if (remoteClients[i].isActive)
        {
            if (systemCount < maxToWrite)
                remoteSystems[systemCount] = remoteClients[i].systemAddress;
            systemCount++;
        }
    }
    *numberOfSystems = systemCount;
}

unsigned short TCPInterface::GetConnectionCount(void) const
{
    unsigned short systemCount = 0;
    for (int i = 0; i < remoteClientsLength; i++)
    {
        if (remoteClients[i].isActive)
            systemCount++;
    }
    return systemCount;
}

unsigned int TCPInterface::GetOutgoingDataBufferSize(SystemAddress systemAddress) const
{
    unsigned bytesWritten = 0;
    if (systemAddress.systemIndex < remoteClientsLength &&
        remoteClients[systemAddress.systemIndex].isActive &&
        remoteClients[systemAddress.systemIndex].systemAddress == systemAddress)
    {
        remoteClients[systemAddress.systemIndex].outgoingDataMutex.Lock();
        bytesWritten = remoteClients[systemAddress.systemIndex].outgoingData.GetBytesWritten();
        remoteClients[systemAddress.systemIndex].outgoingDataMutex.Unlock();
        return bytesWritten;
    }

    for (int i = 0; i < remoteClientsLength; i++)
    {
        if (remoteClients[i].isActive && remoteClients[i].systemAddress == systemAddress)
        {
            remoteClients[i].outgoingDataMutex.Lock();
            bytesWritten += remoteClients[i].outgoingData.GetBytesWritten();
            remoteClients[i].outgoingDataMutex.Unlock();
        }
    }
    return bytesWritten;
}

__TCPSOCKET__ TCPInterface::SocketConnect(const char *host, unsigned short remotePort, unsigned short socketFamily,
                                          const char *bindAddress)
{
#ifdef __native_client__
    return 0;
#else

#if CRABNET_SUPPORT_IPV6 != 1
    (void) socketFamily;

    struct hostent *server = gethostbyname(host);
    if (server == NULL)
        return 0;


    __TCPSOCKET__ sockfd = socket__(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        return 0;

    sockaddr_in serverAddress;
    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(remotePort);


    if (bindAddress && bindAddress[0])
        serverAddress.sin_addr.s_addr = inet_addr__(bindAddress);
    else
        serverAddress.sin_addr.s_addr = INADDR_ANY;

    int sock_opt = 1024 * 256;
    setsockopt__(sockfd, SOL_SOCKET, SO_RCVBUF, (char *) &sock_opt, sizeof(sock_opt));


    memcpy((char *) &serverAddress.sin_addr.s_addr, (char *) server->h_addr, server->h_length);
    blockingSocketListMutex.Lock();
    blockingSocketList.Insert(sockfd);
    blockingSocketListMutex.Unlock();

    // This is blocking
    int connectResult = connect__(sockfd, (struct sockaddr *) &serverAddress, sizeof(struct sockaddr));

#else
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = socketFamily;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[32];
    Itoa(remotePort,portStr,10);
    getaddrinfo(host, portStr, &hints, &res);
    int sockfd = socket__(res->ai_family, res->ai_socktype, res->ai_protocol);
    blockingSocketListMutex.Lock();
    blockingSocketList.Insert(sockfd);
    blockingSocketListMutex.Unlock();
    int connectResult = connect__(sockfd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res); // free the linked-list

#endif // #if CRABNET_SUPPORT_IPV6!=1

    if (connectResult == -1)
    {
        blockingSocketListMutex.Lock();
        unsigned sockfdIndex = blockingSocketList.GetIndexOf(sockfd);
        if (sockfdIndex != (unsigned) -1)
            blockingSocketList.RemoveAtIndexFast(sockfdIndex);
        blockingSocketListMutex.Unlock();

        closesocket__(sockfd);
        return 0;
    }

    return sockfd;
#endif  // __native_client__
}

RAK_THREAD_DECLARATION(RakNet::ConnectionAttemptLoop)
{
    const TCPInterface::ThisPtrPlusSysAddr *s = (TCPInterface::ThisPtrPlusSysAddr *) arguments;

    SystemAddress systemAddress = s->systemAddress;
    TCPInterface *tcpInterface = s->tcpInterface;
    const int newRemoteClientIndex = systemAddress.systemIndex;
    const unsigned short socketFamily = s->socketFamily;

    char str1[64];
    systemAddress.ToString(false, str1);
    __TCPSOCKET__ sockfd = tcpInterface->SocketConnect(str1, systemAddress.GetPort(), socketFamily, s->bindAddress);

    delete s;
    if (sockfd == 0)
    {
        tcpInterface->remoteClients[newRemoteClientIndex].isActiveMutex.Lock();
        tcpInterface->remoteClients[newRemoteClientIndex].SetActive(false);
        tcpInterface->remoteClients[newRemoteClientIndex].isActiveMutex.Unlock();

        tcpInterface->failedConnectionAttemptMutex.Lock();
        tcpInterface->failedConnectionAttempts.Push(systemAddress);
        tcpInterface->failedConnectionAttemptMutex.Unlock();
        return 0;
    }

    tcpInterface->remoteClients[newRemoteClientIndex].socket = sockfd;
    tcpInterface->remoteClients[newRemoteClientIndex].systemAddress = systemAddress;

    // Notify user that the connection attempt has completed.
    if (tcpInterface->threadRunning > 0)
    {
        tcpInterface->completedConnectionAttemptMutex.Lock();
        tcpInterface->completedConnectionAttempts.Push(systemAddress);
        tcpInterface->completedConnectionAttemptMutex.Unlock();
    }
    return 0;

}

RAK_THREAD_DECLARATION(RakNet::UpdateTCPInterfaceLoop)
{
//    const int BUFF_SIZE=8096;
    //char data[ BUFF_SIZE ];
    const unsigned int BUFF_SIZE = 1048576;
    auto data = (char *) malloc(BUFF_SIZE);

#if CRABNET_SUPPORT_IPV6 != 1
    sockaddr_in sockAddr;
    int sockAddrSize = sizeof(sockAddr);
#else
    struct sockaddr_storage sockAddr;
    socklen_t sockAddrSize = sizeof(sockAddr);
#endif

    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 30000;

    auto sts = (TCPInterface *) arguments;
    sts->threadRunning++;

    while (sts->isStarted > 0)
    {
#if OPEN_SSL_CLIENT_SUPPORT == 1
        SystemAddress *sslSystemAddress = sts->startSSL.PopInaccurate();
        if (sslSystemAddress)
        {
            if (sslSystemAddress->systemIndex >= 0 &&
                sslSystemAddress->systemIndex<sts->remoteClientsLength &&
                sts->remoteClients[sslSystemAddress->systemIndex].systemAddress == *sslSystemAddress)
            {
                sts->remoteClients[sslSystemAddress->systemIndex].InitSSL(sts->ctx,sts->meth);
            }
            else
            {
                for (int i = 0; i < sts->remoteClientsLength; i++)
                {
                    sts->remoteClients[i].isActiveMutex.Lock();
                    if (sts->remoteClients[i].isActive && sts->remoteClients[i].systemAddress == *sslSystemAddress)
                    {
                        if (sts->remoteClients[i].ssl==0)
                            sts->remoteClients[i].InitSSL(sts->ctx,sts->meth);
                    }
                    sts->remoteClients[i].isActiveMutex.Unlock();
                }
            }
            sts->startSSL.Deallocate(sslSystemAddress,);
        }
#endif

        // Linux' select__() implementation changes the timeout
        tv.tv_sec = 0;
        tv.tv_usec = 30000;


#ifdef _MSC_VER
#pragma warning( disable : 4127 ) // warning C4127: conditional expression is constant
#endif
        fd_set readFD, exceptionFD, writeFD;
        while (1)
        {
            // Reset readFD, writeFD, and exceptionFD since select seems to clear it
            FD_ZERO(&readFD);
            FD_ZERO(&exceptionFD);
            FD_ZERO(&writeFD);
            __TCPSOCKET__ largestDescriptor = 0; // see select__()'s first parameter's documentation under linux
            if (sts->listenSocket != 0)
            {
                FD_SET(sts->listenSocket, &readFD);
                FD_SET(sts->listenSocket, &exceptionFD);
                largestDescriptor = sts->listenSocket; // @see largestDescriptor def
            }

            for (unsigned int i = 0; i < (unsigned int) sts->remoteClientsLength; i++)
            {
                sts->remoteClients[i].isActiveMutex.Lock();
                if (sts->remoteClients[i].isActive)
                {
                    // calling FD_ISSET with -1 as socket (that�s what 0 is set to) produces a bus error under Linux 64-Bit
                    __TCPSOCKET__ socketCopy = sts->remoteClients[i].socket;
                    if (socketCopy != 0)
                    {
                        FD_SET(socketCopy, &readFD);
                        FD_SET(socketCopy, &exceptionFD);
                        if (sts->remoteClients[i].outgoingData.GetBytesWritten() > 0)
                            FD_SET(socketCopy, &writeFD);
                        if (socketCopy > largestDescriptor) // @see largestDescriptorDef
                            largestDescriptor = socketCopy;
                    }
                }
                sts->remoteClients[i].isActiveMutex.Unlock();
            }

#ifdef _MSC_VER
#pragma warning( disable : 4244 ) // warning C4127: conditional expression is constant
#endif

            if (select__(largestDescriptor + 1, &readFD, &writeFD, &exceptionFD, &tv) <= 0)
                break;

            if (sts->listenSocket != 0 && FD_ISSET(sts->listenSocket, &readFD))
            {
                __TCPSOCKET__ newSock = accept__(sts->listenSocket, (sockaddr *) &sockAddr, (socklen_t *) &sockAddrSize);

                if (newSock != 0)
                {
                    int newRemoteClientIndex;
                    for (newRemoteClientIndex = 0; newRemoteClientIndex < sts->remoteClientsLength; newRemoteClientIndex++)
                    {
                        auto &newRemoteClient = sts->remoteClients[newRemoteClientIndex];
                        newRemoteClient.isActiveMutex.Lock();
                        if (!newRemoteClient.isActive)
                        {
                            newRemoteClient.socket = newSock;

#if CRABNET_SUPPORT_IPV6 != 1
                            newRemoteClient.systemAddress.address.addr4.sin_addr.s_addr = sockAddr.sin_addr.s_addr;
                            newRemoteClient.systemAddress.SetPortNetworkOrder(sockAddr.sin_port);
                            newRemoteClient.systemAddress.systemIndex = newRemoteClientIndex;
#else
                            if (sockAddr.ss_family==AF_INET)
                            {
                                memcpy(&newRemoteClient.systemAddress.address.addr4,(sockaddr_in *)&sockAddr,sizeof(sockaddr_in));
                            //    newRemoteClient.systemAddress.address.addr4.sin_port=ntohs( newRemoteClient.systemAddress.address.addr4.sin_port );
                            }
                            else
                            {
                                memcpy(&newRemoteClient.systemAddress.address.addr6,(sockaddr_in6 *)&sockAddr,sizeof(sockaddr_in6));
                            //    newRemoteClient.systemAddress.address.addr6.sin6_port=ntohs( newRemoteClient.systemAddress.address.addr6.sin6_port );
                            }

#endif // #if CRABNET_SUPPORT_IPV6!=1
                            newRemoteClient.SetActive(true);
                            newRemoteClient.isActiveMutex.Unlock();

                            SystemAddress *newConnectionSystemAddress = sts->newIncomingConnections.Allocate();
                            *newConnectionSystemAddress = newRemoteClient.systemAddress;
                            sts->newIncomingConnections.Push(newConnectionSystemAddress);

                            break;
                        }
                        newRemoteClient.isActiveMutex.Unlock();
                    }
                    /*if (newRemoteClientIndex == -1)
                        closesocket__(sts->listenSocket);*/
                }
#ifdef _DO_PRINTF
                else
                    CRABNET_DEBUG_PRINTF("Error: connection failed\n");
#endif
            }
#ifdef _DO_PRINTF
            else if (sts->listenSocket != 0 && FD_ISSET(sts->listenSocket, &exceptionFD))
            {
                int err;
                int errlen = sizeof(err);
                getsockopt__(sts->listenSocket, SOL_SOCKET, SO_ERROR,(char*)&err, &errlen);
                CRABNET_DEBUG_PRINTF("Socket error %s on listening socket\n", err);
            }
#endif

            for (unsigned int i = 0; i < (unsigned int) sts->remoteClientsLength;)
            {
                if (!sts->remoteClients[i].isActive)
                {
                    i++;
                    continue;
                }
                // calling FD_ISSET with -1 as socket (that's what 0 is set to) produces a bus error under Linux 64-Bit
                __TCPSOCKET__ socketCopy = sts->remoteClients[i].socket;
                if (socketCopy == 0)
                {
                    i++;
                    continue;
                }

                if (FD_ISSET(socketCopy, &exceptionFD))
                {
                    // Connection lost abruptly
                    SystemAddress *lostConnectionSystemAddress = sts->lostConnections.Allocate();
                    *lostConnectionSystemAddress = sts->remoteClients[i].systemAddress;
                    sts->lostConnections.Push(lostConnectionSystemAddress);
                    sts->remoteClients[i].isActiveMutex.Lock();
                    sts->remoteClients[i].SetActive(false);
                    sts->remoteClients[i].isActiveMutex.Unlock();
                }
                else
                {
                    if (FD_ISSET(socketCopy, &readFD))
                    {
                        // if recv returns 0 this was a graceful close
                        int len = sts->remoteClients[i].Recv(data, BUFF_SIZE);

                        if (len > 0)
                        {
                            Packet *incomingMessage = sts->incomingMessages.Allocate();
                            incomingMessage->data = (unsigned char *) malloc(len + 1);
                            RakAssert(incomingMessage->data);
                            memcpy(incomingMessage->data, data, len);
                            // Null terminate this so we can print it out as regular strings.  This is different from RakNet which does not do this.
                            incomingMessage->data[len] = 0;

                            incomingMessage->length = len;
                            incomingMessage->deleteData = true; // actually means came from SPSC, rather than AllocatePacket
                            incomingMessage->systemAddress = sts->remoteClients[i].systemAddress;
                            sts->incomingMessages.Push(incomingMessage);
                        }
                        else
                        {
                            // Connection lost gracefully
                            SystemAddress *lostConnectionSystemAddress = sts->lostConnections.Allocate();
                            *lostConnectionSystemAddress = sts->remoteClients[i].systemAddress;
                            sts->lostConnections.Push(lostConnectionSystemAddress);
                            sts->remoteClients[i].isActiveMutex.Lock();
                            sts->remoteClients[i].SetActive(false);
                            sts->remoteClients[i].isActiveMutex.Unlock();
                            continue;
                        }
                    }
                    if (FD_ISSET(socketCopy, &writeFD))
                    {
                        RemoteClient *rc = &sts->remoteClients[i];
                        rc->outgoingDataMutex.Lock();
                        unsigned int bytesInBuffer = rc->outgoingData.GetBytesWritten();
                        if (bytesInBuffer > 0)
                        {
                            unsigned int bytesSent;
                            unsigned int contiguousLength;
                            char *contiguousBytesPointer = rc->outgoingData.PeekContiguousBytes(&contiguousLength);
                            if (contiguousLength < BUFF_SIZE && contiguousLength < bytesInBuffer)
                            {
                                unsigned int bytesAvailable = bytesInBuffer > BUFF_SIZE ? BUFF_SIZE : bytesInBuffer;
                                rc->outgoingData.ReadBytes(data, bytesAvailable, true);
                                bytesSent = rc->Send(data, bytesAvailable);
                            }
                            else
                                bytesSent = rc->Send(contiguousBytesPointer, contiguousLength);

                            if (bytesSent > 0)
                                rc->outgoingData.IncrementReadOffset(bytesSent);
                        }
                        rc->outgoingDataMutex.Unlock();
                    }

                    i++; // Nothing deleted so increment the index
                }
            }
        }

        // Sleep 0 on Linux monopolizes the CPU
        RakSleep(30);
    }
    sts->threadRunning--;

    free(data);

    return 0;

}

void RemoteClient::SetActive(bool a)
{
    if (isActive != a)
    {
        isActive = a;
        Reset();
        if (!isActive && socket != 0)
        {
            closesocket__(socket);
            socket = 0;
        }
    }
}

void RemoteClient::SendOrBuffer(const char **data, const unsigned int *lengths, const int numParameters)
{
    // True can save memory and buffer copies, but gives worse performance overall
    // Do not use true for the XBOX, as it just locks up
    const bool ALLOW_SEND_FROM_USER_THREAD = false;

    if (!isActive)
        return;
    for (int parameterIndex = 0; parameterIndex < numParameters; parameterIndex++)
    {
        outgoingDataMutex.Lock();
        if (ALLOW_SEND_FROM_USER_THREAD && outgoingData.GetBytesWritten() == 0)
        {
            outgoingDataMutex.Unlock();
            int bytesSent = Send(data[parameterIndex], lengths[parameterIndex]);
            if (bytesSent < (int) lengths[parameterIndex])
            {
                // Push remainder
                outgoingDataMutex.Lock();
                outgoingData.WriteBytes(data[parameterIndex] + bytesSent, lengths[parameterIndex] - bytesSent);
                outgoingDataMutex.Unlock();
            }
        }
        else
        {
            outgoingData.WriteBytes(data[parameterIndex], lengths[parameterIndex]);
            outgoingDataMutex.Unlock();
        }
    }
}

#if OPEN_SSL_CLIENT_SUPPORT == 1
bool RemoteClient::InitSSL(SSL_CTX* ctx, SSL_METHOD *meth)
{
    (void) meth;

    ssl = SSL_new (ctx);
    RakAssert(ssl);
    int res;
    res = SSL_set_fd (ssl, socket);
    if (res!=1)
    {
        printf("SSL_set_fd error: %s\n", ERR_reason_error_string(ERR_get_error()));
        SSL_free(ssl);
        ssl=0;
        return false;
    }
    RakAssert(res==1);
    res = SSL_connect (ssl);
    if (res<0)
    {
        unsigned long err = ERR_get_error();
        printf("SSL_connect error: %s\n", ERR_reason_error_string(err));
        SSL_free(ssl);
        ssl=0;
        return false;
    }
    else if (res==0)
    {
        // The TLS/SSL handshake was not successful but was shut down controlled and by the specifications of the TLS/SSL protocol. Call SSL_get_error() with the return value ret to find out the reason.
        int err = SSL_get_error(ssl, res);
        switch (err)
        {
        case SSL_ERROR_NONE:
            printf("SSL_ERROR_NONE\n");
            break;
        case SSL_ERROR_ZERO_RETURN:
            printf("SSL_ERROR_ZERO_RETURN\n");
            break;
        case SSL_ERROR_WANT_READ:
            printf("SSL_ERROR_WANT_READ\n");
            break;
        case SSL_ERROR_WANT_WRITE:
            printf("SSL_ERROR_WANT_WRITE\n");
            break;
        case SSL_ERROR_WANT_CONNECT:
            printf("SSL_ERROR_WANT_CONNECT\n");
            break;
        case SSL_ERROR_WANT_ACCEPT:
            printf("SSL_ERROR_WANT_ACCEPT\n");
            break;
        case SSL_ERROR_WANT_X509_LOOKUP:
            printf("SSL_ERROR_WANT_X509_LOOKUP\n");
            break;
        case SSL_ERROR_SYSCALL:
            {
                // http://www.openssl.org/docs/ssl/SSL_get_error.html
                char buff[1024];
                unsigned long ege = ERR_get_error();
                if (ege==0 && res==0)
                    printf("SSL_ERROR_SYSCALL EOF in violation of the protocol\n");
                else if (ege==0 && res==-1)
                    printf("SSL_ERROR_SYSCALL %s\n", strerror(errno));
                else
                    printf("SSL_ERROR_SYSCALL %s\n", ERR_error_string(ege, buff));
            }
            break;
        case SSL_ERROR_SSL:
            printf("SSL_ERROR_SSL\n");
            break;
        }

    }

    if (res!=1)
    {
        SSL_free(ssl);
        ssl=0;
        return false;
    }
    return true;
}
void RemoteClient::DisconnectSSL(void)
{
    if (ssl)
        SSL_shutdown (ssl);  /* send SSL/TLS close_notify */
}
void RemoteClient::FreeSSL(void)
{
    if (ssl)
        SSL_free (ssl);
}
int RemoteClient::Send(const char *data, unsigned int length)
{
    if (ssl)
        return SSL_write (ssl, data, length);
    else
        return send__(socket, data, length, 0);
}
int RemoteClient::Recv(char *data, const int dataSize)
{
    if (ssl)
        return SSL_read (ssl, data, dataSize);
    else
        return recv__(socket, data, dataSize, 0);
}
#else

int RemoteClient::Send(const char *data, unsigned int length)
{
#ifdef __native_client__
    return -1;
#else
    return send__(socket, data, length, 0);
#endif
}

int RemoteClient::Recv(char *data, const int dataSize)
{
#ifdef __native_client__
    return -1;
#else
    return recv__(socket, data, dataSize, 0);
#endif
}

#endif

#ifdef _MSC_VER
#pragma warning( pop )
#endif

#endif // _CRABNET_SUPPORT_*
