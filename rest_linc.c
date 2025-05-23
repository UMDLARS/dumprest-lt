

#include <termios.h>
#include <unistd.h>
#include <memory.h>

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>

#define ARRAYSIZE(a) (sizeof(a) / sizeof(a[0]))

#include "config.c"
#include "comm.c"

#define MAX_BLOCK_SIZE 256  // 256 for LINC format(s) & 129 for OS/8 format(s)

unsigned short combine_one_word(const unsigned char* parts) {
    return parts[0] | (((unsigned short)parts[1] & 0xF) << 8);
}

void split_one_word(unsigned char *out, unsigned short word)
{
    out[0] = word & 0xFF;
    out[1] = (word >> 8) & 0xF;
}

void split_two_words(unsigned char* out, const unsigned short *words)
{
    out[0] = words[0] & 0xFF;
    out[1] = ((words[0] >> 8) & 0xF) | (words[1] << 4);
    out[2] = words[1] >> 4;
}

unsigned short read_one_word(int fd)
{
    unsigned char buf[2];
    int count = 2;
    int rc;
    while(count != 0) {
        rc = ser_read(fd, buf + (2 - count), count);
        if(rc > 0) {
            count -= rc;
        }
    }
    return combine_one_word(buf);
}

int main(int argc, char *argv[])
{
    int serial_fd;
    FILE* in_file;
    char filename[256];

    char serial_dev[256];
    long baud;
    int two_stop;

    unsigned char buf[3];
    unsigned short block[MAX_BLOCK_SIZE];
    unsigned short checksum = 0;
    unsigned short pdp_checksum = 0;
    int block_size;
    int count;
    int bad_block;
    int sent = -1;
    int rc;

    enum HOST_FLAG {
        FLAG_FINISH = 0xFE,
        FLAG_SEND_BLOCK = 0xFF,
    };

    enum PDP_FLAG {
        PDP_FLAG_CHKSM_BAD  = 0xFD,
        PDP_FLAG_LEN_BAD    = 0xFE,
        PDP_FLAG_REQ_BLOCK  = 0XFF,
    };

    setup_config(&baud, &two_stop, serial_dev);

    /* If available, read input path from argv; otherwise, ask user for input path.*/
    if(argc > 1) {
        strncpy(filename, argv[1], sizeof(filename) - 1);
    } else {
        printf("Enter file name to send\n");
        fflush(stdout);
        scanf("%s", filename);
    }

    /* Open input file. */
    in_file = fopen(filename, "rb");
    if(in_file == NULL) {
        fprintf(stderr, "On file %s ", filename);
        perror("open failed");
        return 1;
    }

    /* Open serial connection. */
    serial_fd = init_comm(serial_dev, baud, two_stop);
    printf( "opened serial port: %s\n", serial_dev );

    /* Wait for PDP8 program to send device block size. */
    block_size = read_one_word(serial_fd);

    /* Send loop. */
    count = block_size;
    while(1) {
        /* Read another block if at the end of a block. */
        if(count == block_size) {
            /* Wait for a flag from the PDP. */
            while(ser_read(serial_fd, buf, 1) < 1);

            /* Print bad block if bad block flag sent. */
            if(buf[0] == PDP_FLAG_CHKSM_BAD) {
                /* Next word should be the bad block num. */
                bad_block = read_one_word(serial_fd);
                printf("PDP failed to write block %d\n", bad_block);
                continue;
            }

            sent++;
            printf("Sending block %d\n", sent);

            /* Read a block. */
            rc = fread(block, sizeof(block[0]), ARRAYSIZE(block), in_file);

            /* Clear count. */
            count = 0;

            /* Check if we're at the end or encountered an error. */
            if(rc != block_size) {
                if(rc < 0) {
                    perror("Failed to read input file");
                    return 1;
                } else if(rc != 3) { // last 6 bytes/3 WORDS contain block size + other info.
                    printf("Early EOF\n");
                    return 1;
                }

                /* Send finished flag then wait for PDP's checksum. */
                buf[0] = FLAG_FINISH;
                ser_write(serial_fd, buf, 1);
                pdp_checksum = read_one_word(serial_fd);

                /* Mask off upper bits of checksum. */
                checksum &= 0xFFF;

                /* Check the checksum. */
                if(checksum != pdp_checksum) {
                    printf("Checksum mismatch: %d, %d\n", checksum, pdp_checksum);
                }

                /* Exit w/ success. */
                return 0;
            } else {
                /* Tell PDP we're sending another block. */
                buf[0] = FLAG_SEND_BLOCK;
                ser_write(serial_fd, buf, 1);
            }
        }
        
        /* Add first word to checksum. */
        checksum += block[count];
        if(count + 1 == block_size) {
            /* If we're at the last word on an odd block count format, */
            /* send last word as two bytes. */
            split_one_word(buf, block[count]);
            ser_write(serial_fd, buf, 2);
            count++;
        } else {
            /* Otherwise, send two words as three bytes and add second word to the checksum. */
            split_two_words(buf, block + count);
            //printf("%04X/%04o %04X/%04o %04X/%04o %02X %02X %02X\n", count, count, block[count], block[count], block[count + 1], block[count + 1], buf[0], buf[1], buf[2]);
            ser_write(serial_fd, buf, 3);
            checksum += block[count + 1];
            count += 2;
        }
    }
}
