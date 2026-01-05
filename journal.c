#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static void cmd_install(void);

// Filesystem constants

#define FS_MAGIC 0x56534653U

#define BLOCK_SIZE        4096U
#define INODE_SIZE         128U

#define JOURNAL_BLOCK_IDX    1U
#define JOURNAL_BLOCKS      16U

#define INODE_BMAP_IDX     (JOURNAL_BLOCK_IDX + JOURNAL_BLOCKS)
#define DATA_BMAP_IDX      (INODE_BMAP_IDX + 1U)
#define INODE_START_IDX    (DATA_BMAP_IDX + 1U)
#define DATA_START_IDX     (INODE_START_IDX + 2U)

#define DATA_BLOCKS         64U
#define DIRECT_POINTERS      8U

#define DEFAULT_IMAGE "vsfs.img"

// Journal Structures

#define JOURNAL_MAGIC 0x4A524E4C

#define REC_DATA   1
#define REC_COMMIT 2

struct journal_header {
    uint32_t magic;
    uint32_t nbytes_used;
};

struct rec_header {
    uint16_t type;
    uint16_t size;
};

struct data_record {
    struct rec_header hdr;
    uint32_t block_no;
    uint8_t block[BLOCK_SIZE];
};

struct commit_record {
    struct rec_header hdr;
};

struct inode {
    uint16_t type;
    uint16_t links;
    uint32_t size;
    uint32_t direct[DIRECT_POINTERS];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t _pad[128 - (2 + 2 + 4 + DIRECT_POINTERS * 4 + 4 + 4)]; // 
};

struct dirent { 
    uint32_t inode;
    char name[28];
};

//

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

// Read a full block

static void read_block(int fd, uint32_t block_no, void *buf) {
    if (pread(fd, buf, BLOCK_SIZE, (off_t)block_no * BLOCK_SIZE) != BLOCK_SIZE) {
        die("pread");
    }
}

// Write a full block

static void write_block(int fd, uint32_t block_no, const void *buf) {
    if (pwrite(fd, buf, BLOCK_SIZE, (off_t)block_no * BLOCK_SIZE) != BLOCK_SIZE) {
        die("pwrite");
    }
}
//

static int bitmap_test(uint8_t *bm, uint32_t idx) {
    return (bm[idx / 8] >> (idx % 8)) & 1; 
}

static void bitmap_set(uint8_t *bm, uint32_t idx) {
    bm[idx / 8] |= (1 << (idx % 8));
}

// Open Journal & Load Header

static void load_journal_header(int fd, struct journal_header *jh) {
    uint8_t block[BLOCK_SIZE];
    read_block(fd, JOURNAL_BLOCK_IDX, block);
    memcpy(jh, block, sizeof(*jh)); // initialize journal if needed

    if (jh->magic != JOURNAL_MAGIC) { // set up new journal
        jh->magic = JOURNAL_MAGIC;
        jh->nbytes_used = sizeof(struct journal_header); // write back journal header
        memset(block, 0, BLOCK_SIZE); // clear block
        memcpy(block, jh, sizeof(*jh)); // copy header
        write_block(fd, JOURNAL_BLOCK_IDX, block);
    }
}

// Append DATA Record to Journal

static void append_data_record(int fd, struct journal_header *jh, uint32_t block_no, const uint8_t *block_image) {
    struct data_record rec;
    rec.hdr.type = REC_DATA;
    rec.hdr.size = sizeof(rec);
    rec.block_no = block_no;
    memcpy(rec.block, block_image, BLOCK_SIZE);

    off_t offset = (off_t)JOURNAL_BLOCK_IDX * BLOCK_SIZE + jh->nbytes_used;

    if (pwrite(fd, &rec, sizeof(rec), offset) != sizeof(rec)) {
        die("append data");
    }

    jh->nbytes_used += sizeof(rec);
}

// Append Commit Record to Journal

static void append_commit_record(int fd, struct journal_header *jh) { 
    struct commit_record rec;
    rec.hdr.type = REC_COMMIT; 
    rec.hdr.size = sizeof(rec); // calculate offset

    off_t offset = (off_t)JOURNAL_BLOCK_IDX * BLOCK_SIZE + jh->nbytes_used;

    if (pwrite(fd, &rec, sizeof(rec), offset) != sizeof(rec)) {
        die("append commit");
    }

    jh->nbytes_used += sizeof(rec); // update journal header
}

// Persist Journal Header

static void flush_journal_header(int fd, const struct journal_header *jh) { // write back journal header
    uint8_t block[BLOCK_SIZE]; // buffer for block
    memset(block, 0, BLOCK_SIZE); // clear block
    memcpy(block, jh, sizeof(*jh)); // copy header
    write_block(fd, JOURNAL_BLOCK_IDX, block);
}

// create 

static void cmd_create(const char *name) {
    
    cmd_install(); 

    int fd = open(DEFAULT_IMAGE, O_RDWR);  
    if (fd < 0) die("open image"); 

    struct journal_header jh; 
    load_journal_header(fd, &jh);

    // Read existing metadata
    
    uint8_t inode_bitmap[BLOCK_SIZE]; // 
    uint8_t data_bitmap[BLOCK_SIZE];
    read_block(fd, INODE_BMAP_IDX, inode_bitmap);
    read_block(fd, DATA_BMAP_IDX, data_bitmap);

    uint8_t inode_blocks[2 * BLOCK_SIZE]; // 
    read_block(fd, INODE_START_IDX, inode_blocks);
    read_block(fd, INODE_START_IDX + 1, inode_blocks + BLOCK_SIZE);

    struct inode *inodes = (struct inode *)inode_blocks; // inode array

    uint8_t root_dir_block[BLOCK_SIZE];
    read_block(fd, DATA_START_IDX, root_dir_block);

    // Allocate a free inode

    int new_ino = -1;
    for (int i = 0; i < 16; i++) {
        if (!bitmap_test(inode_bitmap, i)) {
            new_ino = i;
            break;
        }
    }

    if (new_ino < 0) {
        fprintf(stderr, "No free inodes\n");
        close(fd);
        return;
    }

    // Initialize inode

    struct inode *ino = &inodes[new_ino];
    memset(ino, 0, sizeof(*ino));

    ino->type = 1; // regular file
    ino->links = 1; // single link
    ino->size = 0; // no data blocks yet

    bitmap_set(inode_bitmap, new_ino);

    // Add directory entry

    struct dirent *dir = (struct dirent *)root_dir_block; // root directory entries
    int added = 0; 

    for (int i = 0; i < BLOCK_SIZE / sizeof(struct dirent); i++) { // find free entry
        if (dir[i].inode == 0) { // free entry
            dir[i].inode = new_ino; // set inode number
            strncpy(dir[i].name, name, sizeof(dir[i].name) - 1); // ensure null-termination
            dir[i].name[sizeof(dir[i].name) - 1] = '\0'; 
            added = 1;
            break;
        }
    }

    if (!added) {
        fprintf(stderr, "Root directory full\n");
        close(fd);
        return;
    }

    // Append journal records

    append_data_record(fd, &jh, INODE_BMAP_IDX, inode_bitmap); 
    append_data_record(fd, &jh, DATA_BMAP_IDX, data_bitmap);
    append_data_record(fd, &jh, INODE_START_IDX, inode_blocks);
    append_data_record(fd, &jh, INODE_START_IDX + 1, inode_blocks + BLOCK_SIZE); 
    append_data_record(fd, &jh, DATA_START_IDX, root_dir_block);    
    append_commit_record(fd, &jh);

    flush_journal_header(fd, &jh);

    close(fd);
    printf("create '%s' committed to journal\n", name);
}

// install 

static void cmd_install(void) { 
    int fd = open(DEFAULT_IMAGE, O_RDWR); 
    if (fd < 0) die("open image"); // Load journal header

    struct journal_header jh; 
    load_journal_header(fd, &jh);

    // Read entire journal region

    uint8_t journal[BLOCK_SIZE * JOURNAL_BLOCKS]; // buffer for journal blocks
    for (int i = 0; i < JOURNAL_BLOCKS; i++) {
        read_block(fd, JOURNAL_BLOCK_IDX + i, journal + i * BLOCK_SIZE);
    }

    uint32_t pos = sizeof(struct journal_header); // Process records

    struct data_record pending[64]; // maximum pending records
    int pending_count = 0; // number of pending records

    while (pos + sizeof(struct rec_header) <= jh.nbytes_used) { // read record header

        struct rec_header *hdr = 
            (struct rec_header *)(journal + pos); // check for end of journal

        if (hdr->size == 0) { // corrupt record
            break;
        }

        if (hdr->type == REC_DATA) { // store pending data record
            memcpy(&pending[pending_count], journal + pos, hdr->size); // increment count
            pending_count++;
        }
        else if (hdr->type == REC_COMMIT) { // apply pending records
            
            // inode bitmap
            for (int i = 0; i < pending_count; i++) { // find inode bitmap record
                if (pending[i].block_no == INODE_BMAP_IDX) {
                    write_block(fd, pending[i].block_no, pending[i].block);
                }
            }

            // data bitmap
            for (int i = 0; i < pending_count; i++) { // find data bitmap record
                if (pending[i].block_no == DATA_BMAP_IDX) {
                    write_block(fd, pending[i].block_no, pending[i].block); 
                }
            }

            // others
            for (int i = 0; i < pending_count; i++) {
                if (pending[i].block_no != INODE_BMAP_IDX && pending[i].block_no != DATA_BMAP_IDX) {
                    write_block(fd, pending[i].block_no, pending[i].block);
                }
            }

            pending_count = 0;
        }

        pos += hdr->size; // move to next record
    }

    // Clear journal

    jh.nbytes_used = sizeof(struct journal_header); // persist cleared journal
    flush_journal_header(fd, &jh);

    close(fd);
    printf("journal installed\n");
}

int main(int argc, char *argv[]) { // parse command-line arguments
    if (argc < 2) { // not enough arguments
        fprintf(stderr, "usage: journal <create name | install>\n");
        return 1;
    }

    if (strcmp(argv[1], "create") == 0) { // create command
        if (argc != 3) { // missing filename
            fprintf(stderr, "create requires filename\n");
            return 1;
        }
        cmd_create(argv[2]); // create file
    }
    else if (strcmp(argv[1], "install") == 0) { // install command
        cmd_install();
    }
    else {
        fprintf(stderr, "unknown command\n");
        return 1;
    }

    return 0;
}

