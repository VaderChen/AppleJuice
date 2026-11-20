#pragma once

static inline unsigned long long shell_build_drive_root(void *ctx, unsigned long long path,
    unsigned unit, unsigned drive, void (*write_unit)(void *, unsigned long long, unsigned, unsigned))
{
    if (path && drive < 26) {
        write_unit(ctx, path, unit, 'A' + drive);
        write_unit(ctx, path + unit, unit, ':');
        write_unit(ctx, path + 2 * unit, unit, '\\');
        write_unit(ctx, path + 3 * unit, unit, 0);
    }
    return path;
}

static inline int shell_drive_number(void *ctx, unsigned long long path, unsigned unit,
    unsigned (*read_unit)(void *, unsigned long long, unsigned))
{
    if (!path) return -1;
    unsigned letter = read_unit(ctx, path, unit);
    /* 只有 W 入口辨識延伸路徑前綴，每一步只讀取判斷所需的碼元。 */
    if (unit == 2 && letter == '\\' && read_unit(ctx, path + 2, unit) == '\\' &&
        read_unit(ctx, path + 4, unit) == '?' && read_unit(ctx, path + 6, unit) == '\\') {
        path += 8;
        letter = read_unit(ctx, path, unit);
    }
    if (!letter || read_unit(ctx, path + unit, unit) != ':') return -1;
    if (letter >= 'a' && letter <= 'z') return (int)(letter - 'a');
    if (letter >= 'A' && letter <= 'Z') return (int)(letter - 'A');
    return -1;
}
