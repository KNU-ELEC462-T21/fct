#include "file_writer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    file_writer_t writer;

    assert(fw_init(&writer, "test_output.bin", 10, 4) == 0);

    /* Write chunks out of order. */
    assert(fw_write_chunk(&writer, 2, "IJ", 2) == 2);
    assert(fw_write_chunk(&writer, 0, "ABCD", 4) == 4);
    assert(fw_write_chunk(&writer, 1, "EFGH", 4) == 4);

    /* Test invalid inputs. */
    assert(fw_write_chunk(&writer, 3, "X", 1) == -1);
    assert(fw_write_chunk(&writer, 0, "AB", 2) == -1);
    assert(fw_write_at(&writer, 9, "XY", 2) == -1);

    fw_close(&writer);

    /* Verify output file. */
    FILE *fp = fopen("test_output.bin", "rb");
    assert(fp != NULL);

    char buf[11] = {0};
    assert(fread(buf, 1, 10, fp) == 10);
    assert(strcmp(buf, "ABCDEFGHIJ") == 0);

    fclose(fp);
    unlink("test_output.bin");

    printf("All File Writer tests passed!\n");

    return 0;
}
