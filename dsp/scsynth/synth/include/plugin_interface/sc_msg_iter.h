/*
    SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
    http://www.audiosynth.com

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
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/


#pragma once

#include "SC_Endian.h"
#include "SC_Types.h"
#include "SC_FloatToInt.h"
#include <string.h>

// return the ptr to the byte after the OSC string.
inline const char* OSCstrskip(const char* str) {
    //	while (str[3]) { str += 4; }
    //	return str + 4;
    do {
        str += 4;
    } while (str[-1]);
    return str;
}

// returns the number of bytes (including padding) for an OSC string.
inline size_t OSCstrlen(const char* strin) { return (size_t)(OSCstrskip(strin) - strin); }

// returns a float, converting an int if necessary
inline float32 OSCfloat(const char* inData) {
    elem32 elem;
    elem.u = sc_ntohl(*(uint32*)inData);
    return elem.f;
}

inline int32 OSCint(const char* inData) { return (int32)sc_ntohl(*(uint32*)inData); }



inline int64 OSCtime(const char* inData) {
    return ((int64)sc_ntohl(*(uint32*)inData) << 32) + (sc_ntohl(*(uint32*)(inData + 4)));
}

inline float64 OSCdouble(const char* inData) {
    elem64 slot;
    slot.i = ((int64)sc_ntohl(*(uint32*)inData) << 32) + (sc_ntohl(*(uint32*)(inData + 4)));
    return slot.f;
}

// SuperSonic: the reader stays inside the message. The arguments end where
// the type tags or the data end, whichever is first: past the last tag
// remain() is 0 and nextTag() is 0, and a read that would pass the end of the
// data (a short value, a blob longer than what is left) reads nothing and
// leaves the iterator at the end. A string the tags promise but the data does
// not hold reads as empty, so a caller that saw an 's' tag always gets a name.
struct sc_msg_iter {
    const char *data, *rdpos, *endpos, *tags;
    int size, count;
    int numTags;

    sc_msg_iter();
    sc_msg_iter(int inSize, const char* inData);
    void init(int inSize, const char* inData);
    int64 gett(int64 defaultValue = 1);
    int32 geti(int32 defaultValue = 0);
    float32 getf(float32 defaultValue = 0.f);
    float64 getd(float64 defaultValue = 0.f);
    const char* gets(const char* defaultValue = 0);
    int32* gets4(char* defaultValue = 0);
    size_t getbsize();
    void getb(char* outData, size_t inSize);
    void skipb();
    size_t remain() {
        if (tags && count >= numTags)
            return 0;
        return (size_t)(endpos - rdpos);
    }

    char nextTag(char defaultTag = 'f') {
        if (!tags)
            return defaultTag;
        return count < numTags ? tags[count] : 0;
    }

private:
    // True when `n` more bytes of data are there to read; otherwise the
    // iterator moves to the end.
    bool fits(size_t n) {
        if ((size_t)(endpos - rdpos) >= n)
            return true;
        rdpos = endpos;
        return false;
    }
    // The byte after the padded string at `str`, or the end when the string
    // has no terminator inside the message.
    const char* skipString(const char* str) const {
        const char* nul = static_cast<const char*>(memchr(str, 0, (size_t)(endpos - str)));
        if (!nul)
            return endpos;
        const char* next = str + ((nul - str) / 4 + 1) * 4;
        return next < endpos ? next : endpos;
    }
    // Whether the string at `str`, terminated at `nul`, can be read a word at a
    // time (Hash and the name tables read up to the first word whose last byte
    // is zero): the word holding the terminator lies inside the message and is
    // padded with zeros.
    bool wholeWords(const char* str, const char* nul) const {
        const char* wordEnd = str + ((nul - str) / 4 + 1) * 4;
        if (wordEnd > endpos)
            return false;
        for (const char* p = nul; p < wordEnd; ++p)
            if (*p)
                return false;
        return true;
    }
    // The size of the blob at rdpos, or -1 when it does not fit what is left.
    int64 blobSize() const {
        if (endpos - rdpos < (ptrdiff_t)sizeof(int32))
            return -1;
        const int32 len = OSCint(rdpos);
        if (len < 0 || (size_t)len > (size_t)(endpos - rdpos) - sizeof(int32))
            return -1;
        return len;
    }
};

inline sc_msg_iter::sc_msg_iter() {}

inline sc_msg_iter::sc_msg_iter(int inSize, const char* inData) { init(inSize, inData); }

inline void sc_msg_iter::init(int inSize, const char* inData) {
    data = inData;
    size = inSize < 0 ? 0 : inSize;
    endpos = data + size;
    count = 0;
    numTags = 0;
    if (size > 0 && data[0] == ',') {
        tags = data + 1;
        // Tags with no terminator inside the message carry no arguments.
        const char* nul = static_cast<const char*>(memchr(tags, 0, (size_t)(endpos - tags)));
        if (nul) {
            numTags = (int)(nul - tags);
            rdpos = skipString(data);
        } else {
            rdpos = endpos;
        }
    } else {
        tags = 0;
        rdpos = data;
    }
}

inline int64 sc_msg_iter::gett(int64 defaultValue) {
    int64 value;
    if (remain() <= 0)
        return defaultValue;
    if (tags) {
        if (tags[count] == 't') {
            if (!fits(sizeof(int64)))
                return defaultValue;
            value = OSCtime(rdpos);
            rdpos += sizeof(int64);
        } else {
            /* this is dangerous, as rdpos is not
               advanced accordingly while count++ takes
                 place */
            value = defaultValue;
        }
    } else {
        if (!fits(sizeof(int64)))
            return defaultValue;
        value = OSCtime(rdpos);
        rdpos += sizeof(int64);
    }
    count++;
    return value;
}

inline int32 sc_msg_iter::geti(int32 defaultValue) {
    int value;
    if (remain() <= 0)
        return defaultValue;
    if (tags) {
        if (tags[count] == 'i') {
            if (!fits(sizeof(int32)))
                return defaultValue;
            value = OSCint(rdpos);
            rdpos += sizeof(int32);
        } else if (tags[count] == 'f') {
            if (!fits(sizeof(float32)))
                return defaultValue;
            value = sc_float_to_int32(OSCfloat(rdpos));
            rdpos += sizeof(float32);
        } else if (tags[count] == 's') {
            /*	value = atoi(rdpos); */
            value = defaultValue;
            rdpos = skipString(rdpos);
        } else if (tags[count] == 'b') {
            value = defaultValue;
            skipb(); // counts the blob
            return value;
        } else {
            /* this is dangerous, as rdpos is not
               advanced accordingly while count++ takes
               place */
            value = defaultValue;
        }
    } else {
        if (!fits(sizeof(int32)))
            return defaultValue;
        value = (int)OSCint(rdpos);
        rdpos += sizeof(int32);
    }
    count++;
    return value;
}

inline float32 sc_msg_iter::getf(float32 defaultValue) {
    float32 value;
    if (remain() <= 0)
        return defaultValue;
    if (tags) {
        if (tags[count] == 'f') {
            if (!fits(sizeof(float32)))
                return defaultValue;
            value = OSCfloat(rdpos);
            rdpos += sizeof(float32);
        } else if (tags[count] == 'd') {
            if (!fits(sizeof(float64)))
                return defaultValue;
            value = static_cast<float32>(OSCdouble(rdpos));
            rdpos += sizeof(float64);
        } else if (tags[count] == 'i') {
            if (!fits(sizeof(int32)))
                return defaultValue;
            value = static_cast<float32>(OSCint(rdpos));
            rdpos += sizeof(int32);
        } else if (tags[count] == 's') {
            /*    value = atof(rdpos); */
            value = defaultValue;
            rdpos = skipString(rdpos);
        } else if (tags[count] == 'b') {
            value = defaultValue;
            skipb(); // counts the blob
            return value;
        } else {
            /* this is dangerous, as rdpos is not
               advanced accordingly while count++ takes
               place */
            value = defaultValue;
        }
    } else {
        if (!fits(sizeof(float32)))
            return defaultValue;
        value = OSCfloat(rdpos);
        rdpos += sizeof(float32);
    }
    count++;
    return value;
}

inline float64 sc_msg_iter::getd(float64 defaultValue) {
    float64 value;
    if (remain() <= 0)
        return defaultValue;
    if (tags) {
        if (tags[count] == 'f') {
            if (!fits(sizeof(float32)))
                return defaultValue;
            value = (float64)OSCfloat(rdpos);
            rdpos += sizeof(float32);
        } else if (tags[count] == 'd') {
            if (!fits(sizeof(float64)))
                return defaultValue;
            value = OSCdouble(rdpos);
            rdpos += sizeof(float64);
        } else if (tags[count] == 'i') {
            if (!fits(sizeof(int32)))
                return defaultValue;
            value = (float64)OSCint(rdpos);
            rdpos += sizeof(int32);
        } else if (tags[count] == 's') {
            /*    value = atof(rdpos); */
            value = defaultValue;
            rdpos = skipString(rdpos);
        } else if (tags[count] == 'b') {
            value = defaultValue;
            skipb(); // counts the blob
            return value;
        } else {
            /* this is dangerous, as rdpos is not
               advanced accordingly while count++ takes
               place */
            value = defaultValue;
        }
    } else {
        if (!fits(sizeof(float64)))
            return defaultValue;
        value = OSCdouble(rdpos);
        rdpos += sizeof(float64);
    }
    count++;
    return value;
}


// What a string tagged in the message reads as when its bytes are missing or
// have no terminator: empty, padded to a whole name.
inline const int32 kOSCEmptyName[1] = { 0 };

inline const char* sc_msg_iter::gets(const char* defaultValue) {
    const char* value;
    if (tags ? count >= numTags : rdpos >= endpos)
        return 0; // no argument left
    if (!tags || tags[count] == 's') {
        if (!memchr(rdpos, 0, (size_t)(endpos - rdpos))) {
            rdpos = endpos;
            value = (const char*)kOSCEmptyName;
        } else {
            value = rdpos;
            rdpos = skipString(rdpos);
        }
    } else {
        value = defaultValue;
    }
    count++;
    return value;
}

inline int32* sc_msg_iter::gets4(char* defaultValue) {
    int32* value;
    if (tags ? count >= numTags : rdpos >= endpos)
        return 0; // no argument left
    if (!tags || tags[count] == 's') {
        const char* nul = static_cast<const char*>(memchr(rdpos, 0, (size_t)(endpos - rdpos)));
        if (!nul) {
            rdpos = endpos;
            value = const_cast<int32*>(kOSCEmptyName);
        } else {
            value = wholeWords(rdpos, nul) ? (int32*)rdpos : const_cast<int32*>(kOSCEmptyName);
            rdpos = skipString(rdpos);
        }
    } else {
        value = (int32*)defaultValue;
    }
    count++;
    return value;
}

inline size_t sc_msg_iter::getbsize() {
    size_t len = 0;
    if (remain() <= 0)
        return 0;
    if (tags) {
        if (tags[count] == 'b') {
            const int64 size = blobSize();
            len = size < 0 ? 0 : (size_t)size;
        } else if (tags[count] == 'm' && endpos - rdpos >= 4)
            len = 4;
    }
    return len;
}

inline void sc_msg_iter::getb(char* outArray, size_t arraySize) {
    if (remain() <= 0 || !tags)
        return;
    size_t len = 0;
    if (tags[count] == 'b') {
        const int64 size = blobSize();
        if (size < 0) {
            rdpos = endpos;
            return;
        }
        len = (size_t)size;
        if (arraySize < len)
            return;
        rdpos += sizeof(int32);
    } else if (tags[count] == 'm') {
        len = 4;
        if (arraySize < len || !fits(len))
            return;
    }
    size_t len4 = (len + 3) & (size_t)-4;
    memcpy(outArray, rdpos, len);
    rdpos = len4 <= (size_t)(endpos - rdpos) ? rdpos + len4 : endpos;
    count++;
}

inline void sc_msg_iter::skipb() {
    if (remain() <= 0 || !tags)
        return;
    size_t len = 0;
    if (tags[count] == 'b') {
        const int64 size = blobSize();
        if (size < 0) {
            rdpos = endpos;
            return;
        }
        len = (size_t)size;
        rdpos += sizeof(int32);
    } else if (tags[count] == 'm')
        len = 4;
    size_t len4 = (len + 3) & (size_t)-4;
    rdpos = len4 <= (size_t)(endpos - rdpos) ? rdpos + len4 : endpos;
    count++;
}
