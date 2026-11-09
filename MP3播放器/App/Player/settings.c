#include "settings.h"
#include "main.h"

/*
 * 用 Flash 最后一个扇区（Sector 11, 0x080E0000~0x080FFFFF）做参数日志。
 * 每次保存追加一条 16 字节记录，写一次只有几百微秒，不会打断音频；
 * 写满约 8000 条后，在下一次上电时整扇区擦除重来。
 * 要求程序本身不超过 0xE0000（当前约 40KB），换芯片时注意扇区编号。
 */

#define SETTINGS_ADDR   0x080E0000u
#define SETTINGS_END    0x08100000u
#define SETTINGS_MAGIC  0xA5C35A3Cu
#define REC_SIZE        16u

typedef struct {
    uint32_t magic;
    uint32_t counter;
    uint8_t  vol;
    uint8_t  track;
    uint16_t sum;
    uint32_t pad;
} settings_rec_t;

static uint32_t write_addr;
static uint32_t counter;

/* 校验和只覆盖前 10 字节（magic/counter/vol/track） */
static uint16_t rec_sum(const settings_rec_t *r)
{
    const uint8_t *p = (const uint8_t *)r;
    uint16_t s = 0;
    uint8_t i;

    for (i = 0; i < 10; i++) {
        s = (uint16_t)(s + p[i]);
    }
    return s;
}

static uint8_t flash_write(uint32_t addr, const void *buf, uint32_t len)
{
    uint32_t i;

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    for (i = 0; i < len; i += 4) {
        uint32_t word = *(const uint32_t *)((const uint8_t *)buf + i);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, word) != HAL_OK) {
            HAL_FLASH_Lock();
            return 0;
        }
    }

    HAL_FLASH_Lock();
    return 1;
}

static uint8_t flash_erase_sector(void)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0;

    HAL_FLASH_Unlock();

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = FLASH_SECTOR_11;
    erase.NbSectors = 1;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK) {
        HAL_FLASH_Lock();
        return 0;
    }

    HAL_FLASH_Lock();
    return 1;
}

uint8_t settings_load(uint8_t *vol, uint8_t *track)
{
    settings_rec_t rec;
    settings_rec_t last;
    uint32_t addr = SETTINGS_ADDR;
    uint8_t found = 0;

    if (vol == 0 || track == 0) return 0;

    while (addr + REC_SIZE <= SETTINGS_END) {
        rec = *(const volatile settings_rec_t *)addr;
        if (rec.magic == 0xFFFFFFFFu) break;

        if (rec.magic == SETTINGS_MAGIC && rec.sum == rec_sum(&rec)) {
            last = rec;
            counter = rec.counter;
            found = 1;
        }
        addr += REC_SIZE;
    }

    write_addr = addr;

    /* 扇区写满：上电时擦掉重来，避免播放中擦除卡顿 */
    if (write_addr + REC_SIZE > SETTINGS_END) {
        if (flash_erase_sector()) {
            write_addr = SETTINGS_ADDR;
            counter = 0;
        }
    }

    if (found) {
        *vol = last.vol;
        *track = last.track;
    }
    return found;
}

uint8_t settings_save(uint8_t vol, uint8_t track)
{
    settings_rec_t rec;

    if (write_addr < SETTINGS_ADDR) {
        write_addr = SETTINGS_ADDR;
    }

    if (write_addr + REC_SIZE > SETTINGS_END) {
        return 0;               /* 正常不会走到，上电时已处理 */
    }

    rec.magic = SETTINGS_MAGIC;
    rec.counter = ++counter;
    rec.vol = vol;
    rec.track = track;
    rec.sum = 0;
    rec.sum = rec_sum(&rec);
    rec.pad = 0xFFFFFFFFu;

    if (!flash_write(write_addr, &rec, sizeof(rec))) {
        return 0;
    }

    write_addr += REC_SIZE;
    return 1;
}
