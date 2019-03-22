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

#ifdef CRABNET_SOCKET_2_INLINE_FUNCTIONS

#ifndef RAKNETSOCKET2_WINDOWS_LINUX_CPP
#define RAKNETSOCKET2_WINDOWS_LINUX_CPP

#if !defined(__native_client__)

#if (defined(__GNUC__)  || defined(__GCCXML__)) && !defined(__WIN32__)
#include <netdb.h>
#endif

#if CRABNET_SUPPORT_IPV6==1

void PrepareAddrInfoHints2(addrinfo *hints)
{
    memset(hints, 0, sizeof (addrinfo)); // make sure the struct is empty
    hints->ai_socktype = SOCK_DGRAM; // UDP sockets
    hints->ai_flags = AI_PASSIVE;     // fill in my IP for me
}

void GetMyIP_Windows_Linux_IPV4And6( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] )
{
    int idx=0;
    char ac[ 80 ];
    int err = gethostname( ac, sizeof( ac ) );
    RakAssert(err != -1);

    struct addrinfo hints;
    struct addrinfo *servinfo=0, *aip;  // will point to the results
    PrepareAddrInfoHints2(&hints);
    getaddrinfo(ac, "", &hints, &servinfo);

    for (idx=0, aip = servinfo; aip != NULL && idx < MAXIMUM_NUMBER_OF_INTERNAL_IDS; aip = aip->ai_next, idx++)
    {
        if (aip->ai_family == AF_INET)
        {
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)aip->ai_addr;
            memcpy(&addresses[idx].address.addr4,ipv4,sizeof(sockaddr_in));
        }
        else
        {
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)aip->ai_addr;
            memcpy(&addresses[idx].address.addr4,ipv6,sizeof(sockaddr_in6));
        }

    }

    freeaddrinfo(servinfo); // free the linked-list

    while (idx < MAXIMUM_NUMBER_OF_INTERNAL_IDS)
    {
        addresses[idx]=UNASSIGNED_SYSTEM_ADDRESS;
        idx++;
    }
}

#else

void GetMyIP_Windows_Linux_IPV4( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] )
{



    int idx=0;
    char ac[ 80 ];
    int err = gethostname( ac, sizeof( ac ) );
    (void) err;
    RakAssert(err != -1);

    struct hostent *phe = gethostbyname( ac );

    if ( phe == 0 )
    {
        RakAssert(phe!=0);
        return ;
    }
    for ( idx = 0; idx < MAXIMUM_NUMBER_OF_INTERNAL_IDS; ++idx )
    {
        if (phe->h_addr_list[ idx ] == 0)
            break;

        memcpy(&addresses[idx].address.addr4.sin_addr,phe->h_addr_list[ idx ],sizeof(struct in_addr));
    }

    while (idx < MAXIMUM_NUMBER_OF_INTERNAL_IDS)
    {
        addresses[idx]=UNASSIGNED_SYSTEM_ADDRESS;
        idx++;
    }

}

#endif // CRABNET_SUPPORT_IPV6==1


void GetMyIP_Windows_Linux( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] )
{
    #if CRABNET_SUPPORT_IPV6==1
        GetMyIP_Windows_Linux_IPV4And6(addresses);
    #else
        GetMyIP_Windows_Linux_IPV4(addresses);
    #endif
}


#endif // Windows and Linux

#endif // file header

#endif // #ifdef CRABNET_SOCKET_2_INLINE_FUNCTIONS
