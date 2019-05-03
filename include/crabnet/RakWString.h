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

#ifndef __RAK_W_STRING_H
#define __RAK_W_STRING_H 

#include "Export.h"
#include "RakNetTypes.h" // int64_t
#include "RakString.h"

#ifdef _WIN32
#include "WindowsIncludes.h"
#endif

namespace CrabNet
{
    /// \brief String class for Unicode
    class RAK_DLL_EXPORT RakWString
    {
    public:
        // Constructors
        RakWString();
        RakWString( const RakString &right );
        RakWString( const wchar_t *input );
        RakWString( const RakWString & right);
        RakWString( const char *input );
        ~RakWString();

        /// Implicit return of wchar_t*
        operator wchar_t* () const {if (c_str) return c_str; return (wchar_t*) L"";}

        /// Same as std::string::c_str
        const wchar_t* C_String() const {if (c_str) return c_str; return (const wchar_t*) L"";}

        /// Assignment operators
        RakWString& operator = ( const RakWString& right );
        RakWString& operator = ( const RakString& right );
        RakWString& operator = ( const wchar_t * const str );
        RakWString& operator = ( wchar_t *str );
        RakWString& operator = ( const char * const str );
        RakWString& operator = ( char *str );

        /// Concatenation
        RakWString& operator +=( const RakWString& right);
        RakWString& operator += ( const wchar_t * const right );
        RakWString& operator += ( wchar_t *right );

        /// Equality
        bool operator==(const RakWString &right) const;

        // Comparison
        bool operator < ( const RakWString& right ) const;
        bool operator <= ( const RakWString& right ) const;
        bool operator > ( const RakWString& right ) const;
        bool operator >= ( const RakWString& right ) const;

        /// Inequality
        bool operator!=(const RakWString &right) const;

        /// Set the value of the string
        void Set( wchar_t *str );

        /// Returns if the string is empty. Also, C_String() would return ""
        bool IsEmpty() const;

        /// Returns the length of the string
        size_t GetLength() const;

        /// Has the string into an unsigned int
        static unsigned long ToInteger(const RakWString &rs);

        /// Compare strings (case sensitive)
        int StrCmp(const RakWString &right) const;

        /// Compare strings (not case sensitive)
        int StrICmp(const RakWString &right) const;

        /// Clear the string
        void Clear();

        /// Print the string to the screen
        void Printf();

        /// Print the string to a file
        void FPrintf(FILE *fp);

        /// Serialize to a bitstream, uncompressed (slightly faster)
        /// \param[out] bs Bitstream to serialize to
        void Serialize(BitStream *bs) const;

        /// Static version of the Serialize function
        static void Serialize(const wchar_t * const str, BitStream *bs);

        /// Deserialize what was written by Serialize
        /// \param[in] bs Bitstream to serialize from
        /// \return true if the deserialization was successful
        bool Deserialize(BitStream *bs);

        /// Static version of the Deserialize() function
        static bool Deserialize(wchar_t *str, BitStream *bs);


    protected:
        wchar_t* c_str;
        size_t c_strCharLength;
    };

}

const CrabNet::RakWString RAK_DLL_EXPORT operator+(const CrabNet::RakWString &lhs, const CrabNet::RakWString &rhs);


#endif