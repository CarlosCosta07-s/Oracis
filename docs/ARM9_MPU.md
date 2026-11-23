# ARM9 CP15 protection and memory attributes

Oracis models the ARM946E-S protection-unit register state used by the
Pokémon Platinum USA Rev 1 ARM9 startup sequence. CP15 reads and writes are
accepted only in privileged ARM9 modes. The cache-type read reports the DS
configuration: 4KB four-way D-cache and 8KB four-way I-cache, with 32-byte
lines.

The modeled CP15 register set includes:

- c2 data/instruction cacheability bitmaps;
- c3 data write-bufferability bitmap;
- c5 standard and extended instruction/data access permissions;
- c6 eight protection-region descriptors, including region-number priority;
- c1 MPU enable and the existing ARM9 TCM/vector controls.

The implementation validates region alignment, size encodings, reserved
fields, and extended access-permission encodings. Access checks resolve the
highest-numbered matching region and verify every byte of a transfer, so a
transfer crossing a region boundary cannot inherit only the first region's
permission.

The guest bus checks ARM9 data reads/writes before MMIO or memory dispatch.
Static dispatch admission checks ARM9 instruction permission in addition to
the existing live-byte identity check. ARM7 does not use this ARM9 MPU. Active
ITCM and DTCM windows use the TCM interface and are not checked as external
memory regions.

The Platinum startup sequence writes c2/c3/c5/c6 before enabling MPU through
c1. The machine test uses those exact register values and verifies readback and
a privileged main-RAM transfer.

## Deliberate limits

Oracis does not yet contain physical I-cache/D-cache storage or an asynchronous
write buffer. Cacheability/write-buffer attributes are retained as CP15 state,
but guest accesses remain coherent and writes complete synchronously. The
supported ARM946E-S c7 cache flush, line clean, clean-and-flush, prefetch, and
write-buffer-drain encodings are therefore ordered no-ops; they do not pretend
to clean or invalidate cache lines that do not exist. Full-cache flush forms
still enforce their required zero operand.

An MPU permission violation is currently rejected at the host bus boundary with
a diagnostic rather than entering the ARM946E-S Data Abort vector. Architectural
Data Abort entry, fault-status/address registers, and exact abort return-PC
semantics remain a separate implementation milestone. Native game execution
must stay disabled until that exception path is implemented and tested.
