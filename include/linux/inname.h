/*
 * Types and definitions for the Name-Oriented Stack Architecture
 * implementation.
 *
 * Authors:
 * Juan Lang <juan.lang@ericsson.com>
 */
#ifndef LINUX_INNAME_H
#define LINUX_INNAME_H

/* According to RFC1034, the maximum number of octets of a transmitted name is
 * 255.  Assuming at least one preceding length octet and one terminating
 * length octet of 0, this implies the maximum text length of domain name is
 * 253 bytes, or 254 bytes with a NULL terminator.
 */
struct name_addr {
    char name[254];
};

struct sockaddr_name {
    unsigned short int sname_family; /* AF_NAME */
    __be16             sname_port;   /* Transport layer port # */
    struct name_addr   sname_addr;
};

#endif /* LINUX_INNAME_H */
