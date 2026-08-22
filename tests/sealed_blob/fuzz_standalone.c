#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv)
{
    int i;
    if (argc < 2)
        return 2;
    for (i = 1; i < argc; ++i)
    {
        FILE *file = fopen(argv[i], "rb");
        long length;
        uint8_t *data;
        if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
            (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0)
            return 2;
        data = (uint8_t *)malloc((size_t)length + 1U);
        if (data == NULL || fread(data, 1U, (size_t)length, file) != (size_t)length)
        {
            free(data);
            fclose(file);
            return 2;
        }
        fclose(file);
        (void)LLVMFuzzerTestOneInput(data, (size_t)length);
        free(data);
    }
    return 0;
}
