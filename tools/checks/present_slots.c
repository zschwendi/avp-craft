/* Copyright 2026 mcopt-shade contributors. Licensed under Apache-2.0. */
#include <assert.h>
#include <stdio.h>
#include "mcpresent_slots.h"
int main(void) {
    _Atomic int busy[3];
    int cases=0;
    for(int mask=0;mask<8;mask++) for(int next=0;next<3;next++) {
        for(int s=0;s<3;s++) atomic_init(&busy[s],(mask>>s)&1);
        int picked=mc_present_available_slot(busy,3,next);
        assert((picked<0)==(mask==7));
        if(picked>=0) {
            assert(picked<3 && !atomic_load(&busy[picked]));
            for(int d=0;d<(picked-next+3)%3;d++) assert(atomic_load(&busy[(next+d)%3]));
        }
        cases++;
    }
    /* GPU is reading slot0. Slot1 was superseded by mailbox slot2 and freed.
       The old next-only implementation drops every new image at next0 here. */
    atomic_store(&busy[0],1); atomic_store(&busy[1],0); atomic_store(&busy[2],1);
    assert(mc_present_available_slot(busy,3,0)==1);
    int next=0, held=0, mailbox=2;
    for(int image=0;image<10000;image++) {
        int slot=mc_present_available_slot(busy,3,next);
        assert(slot>=0 && slot!=held && slot!=mailbox);
        atomic_store(&busy[slot],1);
        next=(slot+1)%3;
        atomic_store(&busy[mailbox],0); /* only superseded, never GPU-read image */
        mailbox=slot;
        assert(atomic_load(&busy[held]) && atomic_load(&busy[mailbox]));
        cases++;
    }
    atomic_store(&busy[1],1); atomic_store(&busy[2],1);
    assert(mc_present_available_slot(busy,3,next)==-1);
    atomic_store(&busy[held],0); /* completion releases the reader */
    assert(mc_present_available_slot(busy,3,next)==held);
    printf("PASS %d slot states/mailbox replacements; held reader preserved; no false full-ring skip\n",cases);
}
