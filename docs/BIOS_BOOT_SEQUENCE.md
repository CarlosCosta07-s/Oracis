# Retail Nintendo DS BIOS boot path

## Reset entry

The retail NDS reset path starts both processors from their own BIOS mappings in
the same hardware reset transition:

- ARM9 starts at `0xFFFF0000`, with its 4 KiB BIOS mapped to the ARM9 bus.
- ARM7 starts at `0x00000000`, with its 16 KiB BIOS mapped to the ARM7 bus.

Both CPU states begin in Supervisor/ARM mode with IRQ and FIQ masked. Both are
runnable under the shared dual-core scheduler from the first BIOS round. The
host must not hold ARM7 until a guessed synchronization value or a synthetic
host-side release event.

The ARM9 BIOS can complete its own preparation earlier than ARM7. Published
system analysis describes the processors synchronizing after their separate
boot routines, with ARM9 sending a four-bit status value and waiting for ARM7
to respond. That is evidence for guest-driven IPC coordination, not enough
evidence to hardcode a universal nibble or release rule in the host runtime.

## IPC synchronization contract

`IPCSYNC` is a 16-bit register at `0x04000180`, independently readable and
writable by both CPUs:

| Bits | Meaning from the accessing CPU |
|---|---|
| 0–3 | Four-bit value last output by the peer |
| 8–11 | Local four-bit output value |
| 13 | Write-one send-IRQ strobe to the peer |
| 14 | Local enable for receiving IPC synchronization IRQs |

The output nibble is visible to the peer independently of the IRQ strobe.
Writing bit 13 requests the remote synchronization IRQ only when the peer has
enabled reception with bit 14. The IRQ is latched in the peer's IF register as
IPC Sync (bit 16); IME, IE and CPSR.I remain separate interrupt-delivery gates.

The FIFO path is separate from IPCSYNC:

- `IPCFIFOCNT`: `0x04000184`
- `IPCFIFOSEND`: `0x04000188`
- `IPCFIFORECV`: `0x04100000`
- Each direction has an independent queue of 16 32-bit words.

IPCSYNC is suitable for short status/doorbell values; the FIFO carries payload
words. Oracis tests the register semantics in both directions, including data
visibility without a strobe, local IRQ-enable behavior, repeated strobes, and
FIFO queue/interrupt behavior.

## What this path does not assume

Oracis does not put a fixed BIOS handshake nibble, timing delay, IPC interrupt,
or ARM7 release into the host. The actual user-provided ARM9 and ARM7 BIOS code
must perform the synchronization through the emulated IPC devices. The precise
sequence can only be confirmed by executing and tracing the corresponding
user-supplied BIOS revision.

The current synthetic integration tests validate CPU reset entry, NDSRecomp
native dispatch, simultaneous scheduling, IPCSYNC signaling semantics, and
shared main-RAM visibility. They do not claim that a retail BIOS or Pokémon
Platinum has booted.

## References

- Rodrigo Copetti, [Nintendo DS Architecture – Operating System](https://classic.copetti.org/writings/consoles/nintendo-ds/), especially “Entry point” and “The next stage”.
- GBATEK, [Nintendo DS technical reference](https://github.com/mgba-emu/gbatek/blob/master/index.md), IPCSYNC and IPC FIFO register descriptions.
- Homebrew Nintendo DS Development, [Part 7 – FIFO](https://double.nz/nintendo_ds/nds_develop7.html), register behavior and peer FIFO semantics.
