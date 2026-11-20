//
// Created by matadoryuki on 13/06/2024.
//

#ifndef INJECTOR_LITE_FREEFIRE_BASE_CONST_H
#define INJECTOR_LITE_FREEFIRE_BASE_CONST_H


#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string>
#include <vector>
#include <errno.h>
#include <stdio.h>
#include <sys/un.h>

// The abstract socket name is not stored as plain text: it is XOR-encoded here and
// rebuilt on first use. Client and payload compile the same header, so they always
// agree, and a plain `strings` over either binary shows nothing meaningful.
namespace bkl_sock {

    static const unsigned char kEnc[] = {
            0xcd, 0xce, 0xd3, 0x89, 0xc6, 0xd5, 0xd3, 0x89,
            0xd0, 0xc8, 0xd5, 0xcc, 0xc2, 0xd5,
    };
    static const unsigned kKey = 0xA7;

    inline const char *name() {
        struct Buf {
            char s[sizeof(kEnc) + 1];

            Buf() {
                for (unsigned i = 0; i < sizeof(kEnc); ++i) s[i] = (char) (kEnc[i] ^ kKey);
                s[sizeof(kEnc)] = '\0';
            }
        };
        static Buf b;
        return b.s;
    }

} // namespace bkl_sock

#define SOCKET_NAME bkl_sock::name()
#define BACKLOG 8
#define PacketSize 4096



#endif //INJECTOR_LITE_FREEFIRE_BASE_CONST_H
