#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PERSON_NAME_LEN 16

typedef struct {
    uint8_t species;
    uint32_t person_id;
    char person_name[PERSON_NAME_LEN]; // no text-entry UI yet -- left blank (step 7 polish)
    uint32_t timestamp;  // seconds since THIS boot -- see storage.c note (no RTC on this hardware)
    uint8_t selfie_slot; // 0xFF = none (selfie pipeline lands in steps 5-6)
    uint8_t flags;
} dex_entry_t;

/* Finds the "storage" partition (partitions.csv) and prepares the dex and
 * met-log regions, erasing/reinitializing either one only if its header is
 * missing or corrupt -- an already-initialized region with valid data is
 * left untouched. */
void storage_init(void);

/* Appends a new dex entry. Returns false if the dex log region is full. */
bool storage_dex_add(uint8_t species, uint32_t person_id, uint32_t timestamp, uint8_t selfie_slot);
int  storage_dex_count(void);
bool storage_dex_get(int index, dex_entry_t *out); // oldest-first

/* 30-minute anti-farming rule (GAME.md section 6). `now` and stored
 * timestamps are both seconds-since-boot: this hardware has no RTC, so the
 * cooldown can't track real elapsed time across a reboot. Comparisons only
 * ever match a timestamp from the *current* boot session (see storage.c),
 * so a reboot safely resets the cooldown instead of misfiring. */
bool storage_recently_caught(uint32_t person_id, uint32_t now, uint32_t window_seconds);
void storage_record_catch(uint32_t person_id, uint32_t now);
