#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/guest_memory_abi.h"
#include "oracis/memory/nds_dma_control.hpp"
#include "oracis/memory/nds_game_card_control.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/recomp/runtime_arm.h"
#include <array>
#include <cstdint>
#include <cstdio>
namespace {
int fail(const char*m){std::fprintf(stderr,"nds_game_card_control_test: %s\n",m);return 1;}
void write_card_command(uint8_t opcode,uint32_t offset){
 using oracis::memory::NdsGameCardControl;
 const uint8_t b[8]={opcode,uint8_t(offset>>24u),uint8_t(offset>>16u),uint8_t(offset>>8u),uint8_t(offset),0u,0u,0u};
 for(uint32_t i=0;i<8u;++i)bus_write_u8(NdsGameCardControl::CommandAddress+i,b[i]);
}
void write_b7(uint32_t offset){write_card_command(oracis::memory::NdsGameCardControl::RomReadCommand,offset);}
void write_key1_command(const std::array<uint8_t,8> &bytes){
 using oracis::memory::NdsGameCardControl;
 for(uint32_t i=0u;i<bytes.size();++i)
  bus_write_u8(NdsGameCardControl::CommandAddress+i,bytes[i]);
}
std::array<uint8_t,8> make_key1_command(uint8_t opcode,uint16_t parameter16,
        uint32_t following24,uint32_t counter20){
 return {{
  uint8_t((opcode<<4u)|((parameter16>>12u)&0x0Fu)),
  uint8_t(parameter16>>4u),
  uint8_t(((parameter16&0x0Fu)<<4u)|((following24>>20u)&0x0Fu)),
  uint8_t(following24>>12u),uint8_t(following24>>4u),
  uint8_t(((following24&0x0Fu)<<4u)|((counter20>>16u)&0x0Fu)),
  uint8_t(counter20>>8u),uint8_t(counter20)
 }};
}
void write_activate_key1(uint16_t random_i,uint16_t random_j,uint32_t counter){
 using oracis::memory::NdsGameCardControl;
 const uint8_t b[8]={NdsGameCardControl::ActivateKey1Command,
     uint8_t(random_i>>4u),
     uint8_t((random_i<<4u)|(random_j>>8u)),uint8_t(random_j),
     uint8_t(0xA0u|((counter>>16u)&0x0Fu)),uint8_t(counter>>8u),
     uint8_t(counter),UINT8_C(0xA5)};
 for(uint32_t i=0u;i<8u;++i)bus_write_u8(NdsGameCardControl::CommandAddress+i,b[i]);
}
}
int main(){
 using namespace oracis::memory;
 GuestMemory memory;
 if(memory.map_ram(0x02000000u,0x1000u,Readable|Writable,"game-card-dma-test")!=Status::Ok)return fail("RAM map failed");
 NdsInterruptControl irq(memory);NdsGameCardControl card(irq);NdsDmaControl dma(memory,irq);
 if(card.install()!=Status::Ok)return fail("install failed");
 dma.attach_game_card_control(card);card.attach_dma_control(dma);
 std::array<uint8_t,0x9000> image{};
 for(uint32_t i=0;i<image.size();++i)image[i]=uint8_t((i*13u+7u)&255u);
 image[0x0Cu]='C';image[0x0Du]='P';image[0x0Eu]='U';image[0x0Fu]='E';
 image[0x13u]=3u;
 constexpr uint32_t header_normal_romctrl=UINT32_C(0x00586000);
 constexpr uint32_t header_key1_romctrl=UINT32_C(0x001808F8);
 for(uint32_t i=0u;i<4u;++i){
  image[NdsGameCardControl::HeaderNormalRomControlOffset+i]=uint8_t(header_normal_romctrl>>(i*8u));
  image[NdsGameCardControl::HeaderKey1RomControlOffset+i]=uint8_t(header_key1_romctrl>>(i*8u));
 }
 image[NdsGameCardControl::HeaderSecureAreaDelayOffset]=UINT8_C(0x1E);
 image[NdsGameCardControl::HeaderSecureAreaDelayOffset+1u]=UINT8_C(0x05);
 if(card.mount_image(image.data(),uint32_t(image.size()))!=Status::Ok)return fail("mount failed");
 if(!card.header_protocol_available()||
    card.header_normal_rom_control()!=header_normal_romctrl||
    card.header_key1_rom_control()!=header_key1_romctrl||
    card.header_key1_gap1_length()!=UINT32_C(0x8F8)||
    card.header_key1_gap2_length()!=UINT32_C(0x18)||
    card.header_key1_initial_gap_bytes()!=UINT32_C(0x910)||
    card.header_key1_gap_clocks_enabled()||card.header_key1_slow_clock()||
    card.header_secure_area_delay()!=UINT16_C(0x051E)||
    card.header_secure_area_delay_core_cycles()!=UINT32_C(0x051E)*UINT32_C(256)||
    card.header_key2_seed_index()!=3u)
     return fail("cartridge header KEY1 timing or KEY2 seed metadata was decoded incorrectly");
 std::array<uint8_t,NdsGameCardKey1::KeyTableBytes> bios_key_table{};
 for(uint32_t i=0u;i<bios_key_table.size();++i)
     bios_key_table[i]=uint8_t((i*37u+11u)&255u);
 if(card.load_key1_key_table(bios_key_table.data(),
        uint32_t(bios_key_table.size()-1u))!=Status::InvalidRegion||
    card.key1_initialized())
     return fail("malformed KEY1 BIOS table was accepted");
 if(card.load_key1_key_table(bios_key_table.data(),
        uint32_t(bios_key_table.size()))!=Status::Ok||
    !card.key1_initialized()||card.key1_id_code()!=UINT32_C(0x45555043))
     return fail("KEY1 table was not initialized from the mounted Game Code");
 ArmCpuState a9{},a7{};a9.cpsr=ORACIS_ARM_MODE_SYSTEM;a7.cpsr=ORACIS_ARM_MODE_SYSTEM;
 if(!oracis_arm_runtime_bind_cpus(&a9,&a7)||!oracis_arm_runtime_select_cpu(NDS_ARM9)||
    !oracis_bus_bind_memory(&memory)||!oracis_bus_bind_interrupt_control(&irq)||
    !oracis_bus_bind_game_card_control(&card)||!oracis_bus_bind_dma_control(&dma))
    return fail("bus bind failed");
 if(oracis_bus_bind_game_card_control(&card)||oracis_bus_unbind_interrupt_control(&irq)||
    oracis_bus_unbind_memory(&memory))return fail("dependency guard failed");
 constexpr uint32_t imask=NdsGameCardControl::GameCardTransferIrqMask;
 bus_write_u16(NdsGameCardControl::AuxSpiControlAddress,uint16_t(NdsGameCardControl::AuxSpiIrqEnableMask));
 auto word=[&](uint32_t o){return uint32_t(image[o])|(uint32_t(image[o+1u])<<8u)|(uint32_t(image[o+2u])<<16u)|(uint32_t(image[o+3u])<<24u);};
 const uint32_t small=NdsGameCardControl::RomCtrlResetReleaseMask|(7u<<NdsGameCardControl::RomCtrlBlockSizeShift);
 const uint32_t normal=NdsGameCardControl::RomCtrlResetReleaseMask|(1u<<NdsGameCardControl::RomCtrlBlockSizeShift);
 if(card.read(NDS_ARM9,NdsGameCardControl::SeedAddress,4u).status!=Status::PermissionDenied)return fail("seed registers were not write-only");
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,small|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::PermissionDenied||card.busy())return fail("Game Card started while the slot was disabled");
 bus_write_u16(NdsGameCardControl::AuxSpiControlAddress,uint16_t(NdsGameCardControl::AuxSpiIrqEnableMask|NdsGameCardControl::AuxSpiSlotEnableMask|NdsGameCardControl::AuxSpiSlotModeMask));
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,small|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::PermissionDenied||card.busy())return fail("parallel ROM read started in SPI mode");
 bus_write_u16(NdsGameCardControl::AuxSpiControlAddress,uint16_t(NdsGameCardControl::AuxSpiIrqEnableMask|NdsGameCardControl::AuxSpiSlotEnableMask));

 /* Seed registers are CPU-local; applying either bank replaces the shared stream. */
 constexpr uint64_t arm9_raw_seed0=UINT64_C(0x91A2B60B1);
 constexpr uint64_t arm9_raw_seed1=UINT64_C(0x5C879B9B05);
 if(card.key2_initialized())return fail("KEY2 stream was not clear after cartridge reset");
 bus_write_u32(NdsGameCardControl::SeedAddress,static_cast<uint32_t>(arm9_raw_seed0));
 bus_write_u32(NdsGameCardControl::SeedAddress+4u,static_cast<uint32_t>(arm9_raw_seed1));
 bus_write_u16(NdsGameCardControl::SeedAddress+8u,static_cast<uint16_t>(0xFF00u|(arm9_raw_seed0>>32u)));
 bus_write_u16(NdsGameCardControl::SeedAddress+10u,static_cast<uint16_t>(0xFF00u|(arm9_raw_seed1>>32u)));
 if(card.key2_initialized()||card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     NdsGameCardControl::RomCtrlKey2SeedApplyMask)!=Status::Ok||
    !card.key2_initialized()||card.key2_state1()!=UINT64_C(0x46836A2C48)||
    card.key2_state2()!=UINT64_C(0x506CECF09D)||card.key2_bytes_processed()!=0u||
    (bus_read_u32(NdsGameCardControl::RomControlAddress)&NdsGameCardControl::RomCtrlKey2SeedApplyMask)!=0u)
     return fail("ARM9 KEY2 seed registers were not applied on ROMCTRL.bit15");

 /* A separate ARM7 seed bank updates the same active KEY2 stream. */
 if(!oracis_arm_runtime_select_cpu(NDS_ARM7))return fail("ARM7 select failed for KEY2 seeds");
 constexpr uint64_t arm7_raw_seed0=UINT64_C(0x123456789A);
 constexpr uint64_t arm7_raw_seed1=UINT64_C(0x23456789AB);
 bus_write_u32(NdsGameCardControl::SeedAddress,static_cast<uint32_t>(arm7_raw_seed0));
 bus_write_u32(NdsGameCardControl::SeedAddress+4u,static_cast<uint32_t>(arm7_raw_seed1));
 bus_write_u16(NdsGameCardControl::SeedAddress+8u,static_cast<uint16_t>(0xFF00u|(arm7_raw_seed0>>32u)));
 bus_write_u16(NdsGameCardControl::SeedAddress+10u,static_cast<uint16_t>(0xFF00u|(arm7_raw_seed1>>32u)));
 if(card.write(NDS_ARM7,NdsGameCardControl::RomControlAddress,4u,
       NdsGameCardControl::RomCtrlKey2SeedApplyMask)!=Status::Ok||
    card.key2_state1()!=UINT64_C(0x2C8F351624)||
    card.key2_state2()!=UINT64_C(0x6AC8F35162))
     return fail("ARM7 KEY2 seed bank did not replace the active stream");
 if(!oracis_arm_runtime_select_cpu(NDS_ARM9))return fail("ARM9 restore failed after KEY2 seed test");
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
       NdsGameCardControl::RomCtrlKey2SeedApplyMask)!=Status::Ok||
    card.key2_state1()!=UINT64_C(0x46836A2C48)||
    card.key2_state2()!=UINT64_C(0x506CECF09D))
     return fail("ARM9 KEY2 seed bank was overwritten by ARM7 writes");

 /* The raw 90 chip ID is not guessed from the .nds image; configuration is required. */
 write_card_command(NdsGameCardControl::ChipIdReadCommand,0u);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     small|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::PermissionDenied||card.busy())
     return fail("90 chip ID command started without configured physical metadata");
 constexpr uint32_t synthetic_chip_id=UINT32_C(0x800000C2);
 if(card.set_chip_id(synthetic_chip_id)!=Status::Ok||!card.chip_id_configured()||
    card.chip_id()!=synthetic_chip_id||!card.newer_protocol_variant())
     return fail("synthetic chip ID configuration failed");
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     small|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok)
     return fail("90 chip ID command did not start after explicit configuration");
 if(card.set_chip_id(UINT32_C(0x12345678))!=Status::PermissionDenied)
     return fail("chip ID was mutable during an active card transaction");
 card.advance_bus_cycles(40u);
 if(bus_read_u32(NdsGameCardControl::DataAddress)!=synthetic_chip_id||card.busy()||
    (bus_read_u32(NdsInterruptControl::IfAddress)&imask)==0u)
     return fail("90 chip ID did not return the configured raw value");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)
     return fail("could not clear 90 chip ID IRQ");

 /* Reset-time 9F dummy command returns an 8 KiB high-Z stream (read as FF). */
 write_card_command(NdsGameCardControl::DummyCommand,0u);
 const uint32_t dummy_control=NdsGameCardControl::RomCtrlResetReleaseMask|
     (5u<<NdsGameCardControl::RomCtrlBlockSizeShift);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     dummy_control|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok)
     return fail("9F dummy command did not start");
 card.advance_bus_cycles(40u);
 for(uint32_t word_index=0u;word_index<2048u;++word_index){
     if(word_index!=0u)card.advance_bus_cycles(20u);
     if(bus_read_u32(NdsGameCardControl::DataAddress)!=UINT32_MAX)
         return fail("9F dummy command did not return high-Z/FF data");
 }
 if(card.busy()||(bus_read_u32(NdsInterruptControl::IfAddress)&imask)==0u)
     return fail("9F dummy command did not complete with IRQ");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)
     return fail("could not clear dummy command IRQ");

 /* Initial 00 command returns the raw 0x200-byte cartridge header. */
 write_card_command(NdsGameCardControl::HeaderReadCommand,0u);
 const uint32_t header_control=NdsGameCardControl::RomCtrlResetReleaseMask|
     (1u<<NdsGameCardControl::RomCtrlBlockSizeShift);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     header_control|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok)
     return fail("00 header command did not start");
 card.advance_bus_cycles(40u);
 for(uint32_t word_index=0u;word_index<128u;++word_index){
     if(word_index!=0u)card.advance_bus_cycles(20u);
     if(bus_read_u32(NdsGameCardControl::DataAddress)!=word(word_index*4u))
         return fail("00 header read returned incorrect data");
 }
 if(card.busy()||(bus_read_u32(NdsInterruptControl::IfAddress)&imask)==0u)
     return fail("00 header read did not complete with IRQ");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)
     return fail("could not clear header command IRQ");

 /* Retail B7 redirects secure-range addresses into the main data region. */
 write_b7(0x7FFCu);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     small|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok)
     return fail("B7 secure-range address was not redirected");
 card.advance_bus_cycles(40u);
 if(bus_read_u32(NdsGameCardControl::DataAddress)!=word(0x81FCu)||card.busy())
     return fail("B7 secure-range redirect did not target 0x8000+(addr&0x1FF)");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)
     return fail("could not clear redirected-read IRQ");

 /* B7 is byte-addressed and allows unaligned starting offsets. */
 write_b7(0x8022u);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     small|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok)
     return fail("B7 rejected an unaligned byte address");
 card.advance_bus_cycles(40u);
 if(bus_read_u32(NdsGameCardControl::DataAddress)!=word(0x8022u)||card.busy())
     return fail("unaligned B7 word did not preserve byte order");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)
     return fail("could not clear unaligned-read IRQ");

 /* A page crossing 0x1000 wraps within the same 4 KiB window. */
 write_b7(0x8F00u);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     normal|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok)
     return fail("B7 page crossing a 4 KiB boundary did not start");
 card.advance_bus_cycles(40u+127u*20u);
 for(uint32_t index=0u;index<128u;++index){
     const uint32_t expected_offset=index<64u
         ? 0x8F00u+index*4u : 0x8000u+(index-64u)*4u;
     if(bus_read_u32(NdsGameCardControl::DataAddress)!=word(expected_offset))
         return fail("B7 stream did not wrap at the current 4 KiB window");
     if(index+1u<128u)card.advance_bus_cycles(20u);
 }
 if(card.busy()||(bus_read_u32(NdsInterruptControl::IfAddress)&imask)==0u)
     return fail("wrapped B7 page did not complete");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)
     return fail("could not clear wrapped-page IRQ");

 /* One-word B7 read uses a legal post-secure-area address. */
 write_b7(0x8020u);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     small|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok||!card.busy())
     return fail("B7 transaction failed to start after slot enable");
 if(bus_read_u32(NdsGameCardControl::RomControlAddress)&NdsGameCardControl::RomCtrlDrqMask)
     return fail("early DRQ");
 card.advance_bus_cycles(39u);
 if(bus_read_u32(NdsGameCardControl::RomControlAddress)&NdsGameCardControl::RomCtrlDrqMask)
     return fail("first word latency wrong");
 card.advance_bus_cycles(1u);
 if(!(bus_read_u32(NdsGameCardControl::RomControlAddress)&NdsGameCardControl::RomCtrlDrqMask))
     return fail("DRQ not set");
 if(bus_read_u32(NdsGameCardControl::DataAddress)!=word(0x8020u)||card.busy()||
    ((bus_read_u32(NdsGameCardControl::RomControlAddress)&
       (NdsGameCardControl::RomCtrlStartBusyMask|NdsGameCardControl::RomCtrlDrqMask))!=0u)||
    !(bus_read_u32(NdsInterruptControl::IfAddress)&imask))
     return fail("one-word B7 read or completion IRQ failed");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)
     return fail("clear B7 IRQ failed");

 /* A normal B7 page is 0x200 bytes, word-ready and manually consumable. */
 write_b7(0x8200u);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     normal|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok)
     return fail("0x200-byte B7 page did not start");
 card.advance_bus_cycles(40u);
 for(uint32_t n=0u;n<128u;++n){
     if(n&&!(bus_read_u32(NdsGameCardControl::RomControlAddress)&NdsGameCardControl::RomCtrlDrqMask))
         return fail("DRQ not ready for page word");
     if(bus_read_u32(NdsGameCardControl::DataAddress)!=word(0x8200u+n*4u))
         return fail("B7 page data mismatch");
     if(n<127u)card.advance_bus_cycles(20u);
 }
 if(card.busy()||(bus_read_u32(NdsInterruptControl::IfAddress)&imask)==0u)
     return fail("128-word B7 page did not complete");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)
     return fail("clear page IRQ failed");

 /* ARM9 Game Card DMA timing 5 streams a 0x200-byte page. */
 constexpr uint32_t dma0=NdsDmaControl::FirstRegisterAddress,dma1=dma0+12u;
 constexpr uint32_t arm9_dma_destination=0x02000000u;
 write_b7(0x8400u);
 bus_write_u32(dma0,NdsGameCardControl::DataAddress);
 bus_write_u32(dma0+4u,arm9_dma_destination);
 bus_write_u32(dma0+8u,1u|(2u<<NdsDmaControl::SourceControlShift)|(5u<<27u)|
     NdsDmaControl::RepeatMask|NdsDmaControl::WordTransferMask|
     NdsDmaControl::IrqEnableMask|NdsDmaControl::EnableMask);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     normal|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok)
     return fail("ARM9 DMA card command failed");
 card.advance_bus_cycles(40u+127u*20u);
 for(uint32_t index=0u;index<128u;++index){
     if(memory.read(NDS_ARM9,arm9_dma_destination+index*4u,4u).value!=word(0x8400u+index*4u))
         return fail("ARM9 Game Card DMA page word mismatch");
 }
 if(card.busy()||
    (bus_read_u32(dma0+8u)&NdsDmaControl::EnableMask)==0u||
    bus_read_u32(dma0)!=NdsGameCardControl::DataAddress||
    bus_read_u32(dma0+4u)!=arm9_dma_destination||
    (irq.read(NDS_ARM9,NdsInterruptControl::IfAddress,4u).value&((1u<<8u)|imask))!=((1u<<8u)|imask))
     return fail("ARM9 Game Card DMA did not stream 128 words or preserve programmed state");
 bus_write_u16(dma0+10u,0u);
 if(card.busy())return fail("ARM9 page transfer did not complete");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)
     return fail("clear Game Card IRQ before ARM7 test failed");

 /* ARM7 Game Card DMA timing 2 uses its independent DMA register bank. */
 if(!oracis_arm_runtime_select_cpu(NDS_ARM7))return fail("ARM7 select failed");
 write_b7(0x8800u);
 bus_write_u32(dma1,NdsGameCardControl::DataAddress);
 bus_write_u32(dma1+4u,0x02000004u);
 bus_write_u32(dma1+8u,1u|(2u<<NdsDmaControl::SourceControlShift)|(2u<<28u)|
     NdsDmaControl::RepeatMask|NdsDmaControl::WordTransferMask|
     NdsDmaControl::IrqEnableMask|NdsDmaControl::EnableMask);
 if(card.write(NDS_ARM7,NdsGameCardControl::RomControlAddress,4u,
     small|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok)
     return fail("ARM7 DMA card command failed");
 card.advance_bus_cycles(40u);
 if(memory.read(NDS_ARM7,0x02000004u,4u).value!=word(0x8800u)||card.busy()||
    (bus_read_u32(dma1+8u)&NdsDmaControl::EnableMask)==0u||
    bus_read_u32(dma1)!=NdsGameCardControl::DataAddress||
    bus_read_u32(dma1+4u)!=0x02000004u||
    (irq.read(NDS_ARM7,NdsInterruptControl::IfAddress,4u).value&((1u<<9u)|imask))!=((1u<<9u)|imask))
     return fail("ARM7 DMA did not consume its Game Card DRQ word");
 bus_write_u16(dma1+10u,0u);
 if(card.busy())return fail("ARM7 one-word transfer did not complete");
 if(!oracis_arm_runtime_select_cpu(NDS_ARM9))return fail("ARM9 restore failed");

 /* A B7 range check fails before the cartridge starts a transfer. */
 write_b7(0x9000u);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     normal|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::AddressOverflow||card.busy())
     return fail("out-of-range B7 read was not rejected transactionally");
 /* KEY2 data reads decrypt each returned byte and preserve stream state across commands. */
 NdsGameCardKey2 expected_key2;
 if(expected_key2.set_raw_seeds(arm9_raw_seed0,arm9_raw_seed1)!=Status::Ok)
     return fail("could not prepare independent KEY2 reference stream");
 uint8_t expected_bytes[8]{};
 for(uint32_t i=0u;i<8u;++i)expected_bytes[i]=image[0x8000u+i];
 if(expected_key2.apply_keystream(expected_bytes,4u)!=Status::Ok)
     return fail("could not advance first KEY2 reference word");
 write_b7(0x8000u);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     small|NdsGameCardControl::RomCtrlKey2DataMask|
     NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok||!card.busy())
     return fail("seeded KEY2 data read was not accepted");
 if(card.read_data(NDS_ARM9).status!=Status::PermissionDenied||
    card.key2_bytes_processed()!=0u)
     return fail("KEY2 stream advanced before the first data word became ready");
 card.advance_bus_cycles(40u);
 const uint32_t expected_first=uint32_t(expected_bytes[0])|
     (uint32_t(expected_bytes[1])<<8u)|(uint32_t(expected_bytes[2])<<16u)|
     (uint32_t(expected_bytes[3])<<24u);
 if(bus_read_u32(NdsGameCardControl::DataAddress)!=expected_first||card.busy()||
    card.key2_bytes_processed()!=4u)
     return fail("KEY2 data word was not decrypted with the active stream");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)
     return fail("could not clear first KEY2 data IRQ");
 if(expected_key2.apply_keystream(expected_bytes+4u,4u)!=Status::Ok)
     return fail("could not advance second KEY2 reference word");
 write_b7(0x8004u);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     small|NdsGameCardControl::RomCtrlKey2DataMask|
     NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok)
     return fail("second KEY2 data read did not start");
 card.advance_bus_cycles(40u);
 const uint32_t expected_second=uint32_t(expected_bytes[4])|
     (uint32_t(expected_bytes[5])<<8u)|(uint32_t(expected_bytes[6])<<16u)|
     (uint32_t(expected_bytes[7])<<24u);
 if(bus_read_u32(NdsGameCardControl::DataAddress)!=expected_second||card.busy()||
    card.key2_bytes_processed()!=8u)
     return fail("KEY2 stream did not continue across consecutive card commands");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)
     return fail("could not clear second KEY2 data IRQ");

 /* The Game Card DMA DRQ path must consume the same encrypted word stream. */
 uint8_t expected_dma_bytes[4]={image[0x8400u],image[0x8401u],image[0x8402u],image[0x8403u]};
 if(expected_key2.apply_keystream(expected_dma_bytes,4u)!=Status::Ok)
     return fail("could not advance KEY2 reference stream for DMA");
 const uint32_t expected_dma_word=uint32_t(expected_dma_bytes[0])|
     (uint32_t(expected_dma_bytes[1])<<8u)|(uint32_t(expected_dma_bytes[2])<<16u)|
     (uint32_t(expected_dma_bytes[3])<<24u);
 constexpr uint32_t encrypted_dma_destination=0x02000100u;
 write_b7(0x8400u);
 bus_write_u32(dma0,NdsGameCardControl::DataAddress);
 bus_write_u32(dma0+4u,encrypted_dma_destination);
 bus_write_u32(dma0+8u,1u|(2u<<NdsDmaControl::SourceControlShift)|
     (5u<<27u)|NdsDmaControl::WordTransferMask|NdsDmaControl::EnableMask);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     small|NdsGameCardControl::RomCtrlKey2DataMask|
     NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok)
     return fail("KEY2 Game Card DMA transaction did not start");
 card.advance_bus_cycles(40u);
 if(memory.read(NDS_ARM9,encrypted_dma_destination,4u).value!=expected_dma_word||
    card.busy()||card.key2_bytes_processed()!=12u||
    (bus_read_u32(dma0+8u)&NdsDmaControl::EnableMask)!=0u)
     return fail("Game Card DMA bypassed or corrupted the KEY2 data stream");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)
     return fail("could not clear encrypted Game Card DMA IRQ");

 write_b7(0x8000u);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     small|NdsGameCardControl::RomCtrlKey2DataMask|
     NdsGameCardControl::RomCtrlKey2CommandMask|
     NdsGameCardControl::RomCtrlStartBusyMask)!=Status::PermissionDenied||card.busy())
     return fail("unsupported KEY2 command mode did not remain fail-closed");

 /* 3C has zero defined reply bytes and immediately activates KEY1. */
 constexpr uint16_t key1_random_i=UINT16_C(0xABC),key1_random_j=UINT16_C(0x123);
 constexpr uint32_t key1_counter=UINT32_C(0x54321);
 write_activate_key1(key1_random_i,key1_random_j,key1_counter);
 const uint32_t no_reply_control=NdsGameCardControl::RomCtrlResetReleaseMask;
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     normal|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::PermissionDenied||
    card.key1_command_mode()||card.busy())
     return fail("3C activation accepted a response length other than zero or one explicit word");
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     no_reply_control|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok||
    card.busy()||card.data_ready()||card.remaining_words()!=0u||
    !card.key1_command_mode())
     return fail("zero-length 3C did not activate KEY1 immediately");
 if(card.key1_random_i()!=key1_random_i||card.key1_random_j()!=key1_random_j||
    card.key1_command_counter()!=key1_counter)
     return fail("raw 3C i/j/k fields were not decoded from their packed bit positions");
 write_b7(0x8000u);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     small|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::PermissionDenied||card.busy())
     return fail("raw B7 command was accepted after KEY1 activation");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)
     return fail("could not clear KEY1 activation IRQ");

 /* A fresh reset can also clock one explicit HIGH-Z word after 3C. */
 card.reset_transfer();
 if(card.key1_initialized()||card.key1_command_mode()||
    card.load_key1_key_table(bios_key_table.data(),
        static_cast<uint32_t>(bios_key_table.size()))!=Status::Ok||
    !card.key1_initialized())
     return fail("Game Card reset did not preserve and reinitialize the external KEY1 table");
 bus_write_u16(NdsGameCardControl::AuxSpiControlAddress,
     uint16_t(NdsGameCardControl::AuxSpiIrqEnableMask|
              NdsGameCardControl::AuxSpiSlotEnableMask));
 write_activate_key1(key1_random_i,key1_random_j,key1_counter);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     small|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok||
    !card.busy()||card.key1_command_mode())
     return fail("explicit four-byte 3C HIGH-Z transfer did not start");
 card.advance_bus_cycles(40u);
 if(!card.data_ready()||!card.busy())
     return fail("explicit four-byte 3C response did not become ready");
 const ReadResult high_z_word=card.read_data(NDS_ARM9);
 if(high_z_word.status!=Status::Ok||high_z_word.value!=UINT32_MAX||
    card.busy()||!card.key1_command_mode()||
    card.key1_command_counter()!=key1_counter)
     return fail("four-byte 3C HIGH-Z response did not complete KEY1 activation");
 if(card.set_chip_id(UINT32_C(0x000000C2))!=Status::PermissionDenied)
     return fail("Chip ID protocol variant changed after KEY1 activation");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)
     return fail("could not clear four-byte KEY1 activation IRQ");
 /* The stateful KEY1 path validates k, bounded repeats and the header delay.
  * Data transfer dispatch remains outside this command-sequence helper. */
 NdsGameCardKey1 command_codec;
 if(command_codec.load_key_table(bios_key_table.data(),
        static_cast<uint32_t>(bios_key_table.size()))!=Status::Ok||
    command_codec.initialize(UINT32_C(0x45555043),2u,8u)!=Status::Ok)
     return fail("could not initialize a reference KEY1 command codec");
 NdsGameCardKey1Command decoded_key1{};
 auto encrypted_key2_activation=make_key1_command(
     0x04u,UINT16_C(0x1234),UINT32_C(0xABCDEF),key1_counter+1u);
 if(command_codec.encrypt_command(encrypted_key2_activation.data(),8u)!=Status::Ok)
     return fail("could not encrypt synthetic KEY1 0x40 command");
 write_key1_command(encrypted_key2_activation);
 if(card.decode_next_key1_command(decoded_key1)!=Status::Ok||
    decoded_key1.kind!=NdsGameCardKey1CommandKind::ActivateKey2||
    decoded_key1.key2_seed24!=UINT32_C(0xABCDEF)||
    card.key1_command_counter()!=key1_counter+1u)
     return fail("new KEY1 command was not decoded and counted at k+1");
 if(card.complete_key1_command_gap(1u)!=Status::PermissionDenied||
    card.complete_key1_command_gap(0u)!=Status::Ok||card.key2_initialized())
     return fail("newer 0x40 gap accepted a wrong clock count or activated KEY2 too early");
 if(card.key1_repeat_delay_cycles_remaining()!=card.header_secure_area_delay_core_cycles()||
    card.decode_next_key1_command(decoded_key1)!=Status::PermissionDenied)
     return fail("newer cart allowed KEY1 retransmission before its header secure delay");
 const uint32_t key1_secure_delay=card.header_secure_area_delay_core_cycles();
 card.advance_bus_cycles(key1_secure_delay-1u);
 if(card.key1_repeat_delay_cycles_remaining()!=1u||
    card.decode_next_key1_command(decoded_key1)!=Status::PermissionDenied)
     return fail("KEY1 secure delay expired one bus cycle too early");
 card.advance_bus_cycles(1u);
 if(card.key1_repeat_delay_cycles_remaining()!=0u||
    card.decode_next_key1_command(decoded_key1)!=Status::Ok||
    card.key1_command_counter()!=key1_counter+1u)
     return fail("newer cart did not accept KEY1 retransmission after exact header delay");
 NdsGameCardKey2 expected_key2_from_40;
 if(expected_key2_from_40.initialize_from_random_seed(UINT32_C(0xABCDEF),3u)!=Status::Ok||
    card.complete_key1_command_gap(0u)!=Status::Ok||!card.key2_initialized()||
    card.key2_state1()!=expected_key2_from_40.state1()||
    card.key2_state2()!=expected_key2_from_40.state2()||
    card.key2_bytes_processed()!=0u||card.key1_key2_activation_pending())
     return fail("0x40 gap did not activate KEY2 from its random seed and header index");
 if(card.decode_next_key1_command(decoded_key1)!=Status::PermissionDenied)
     return fail("newer cart accepted more than two transmissions of a normal KEY1 command");

 const uint32_t secure_counter=key1_counter+2u;
 const uint32_t packed_randoms=(uint32_t(key1_random_i)<<12u)|key1_random_j;
 auto encrypted_secure_block=make_key1_command(
     0x02u,UINT16_C(0x0006),packed_randoms,secure_counter);
 if(command_codec.encrypt_command(encrypted_secure_block.data(),8u)!=Status::Ok)
     return fail("could not encrypt synthetic KEY1 Secure Area command");
 write_key1_command(encrypted_secure_block);
 if(card.decode_next_key1_command(decoded_key1)!=Status::Ok||
    decoded_key1.kind!=NdsGameCardKey1CommandKind::ReadSecureAreaBlock||
    decoded_key1.secure_area_block!=6u||
    decoded_key1.random_i!=key1_random_i||decoded_key1.random_j!=key1_random_j||
    card.key1_command_counter()!=secure_counter)
     return fail("KEY1 secure-area command did not validate its block, i/j and k");
 if(card.complete_key1_command_gap(0u)!=Status::Ok||
    card.key1_repeat_delay_cycles_remaining()!=card.header_secure_area_delay_core_cycles()||
    card.decode_next_key1_command(decoded_key1)!=Status::PermissionDenied)
     return fail("newer cart allowed secure-area repeat before its delay");
 card.advance_bus_cycles(key1_secure_delay);
 if(card.key1_repeat_delay_cycles_remaining()!=0u)
     return fail("Secure Area delay countdown did not expire at the header limit");
 for(uint32_t repeat=0u;repeat<8u;++repeat){
  if(card.decode_next_key1_command(decoded_key1)!=Status::Ok||
     card.key1_command_counter()!=secure_counter||
     card.complete_key1_command_gap(0u)!=Status::Ok)
      return fail("newer cart did not accept all eight secure-area data repetitions and gaps");
 }
 if(card.decode_next_key1_command(decoded_key1)!=Status::PermissionDenied)
     return fail("newer cart accepted more than nine secure-area command transmissions");

 /* The llll value from 0x40 is reused by 0x10, 0x60 and 0xA0. */
 const uint32_t chip_id_counter=secure_counter+1u;
 auto encrypted_key1_chip_id=make_key1_command(
     0x01u,UINT16_C(0x1234),packed_randoms,chip_id_counter);
 if(command_codec.encrypt_command(encrypted_key1_chip_id.data(),8u)!=Status::Ok)
     return fail("could not encrypt KEY1 0x10 command");
 write_key1_command(encrypted_key1_chip_id);
 if(card.decode_next_key1_command(decoded_key1)!=Status::Ok||
    decoded_key1.kind!=NdsGameCardKey1CommandKind::ReadChipId||
    card.key1_random_l()!=UINT16_C(0x1234)||
    !card.key1_random_l_initialized()||
    card.complete_key1_command_gap(0u)!=Status::Ok)
     return fail("KEY1 0x10 did not validate llll or complete its first gap");
 card.advance_bus_cycles(key1_secure_delay);
 if(card.decode_next_key1_command(decoded_key1)!=Status::Ok||
    card.complete_key1_command_gap(0u)!=Status::Ok)
     return fail("KEY1 0x10 second transmission/gap failed after secure delay");

 const uint32_t disable_counter=chip_id_counter+1u;
 auto wrong_l_disable=make_key1_command(
     0x06u,UINT16_C(0x5678),packed_randoms,disable_counter);
 if(command_codec.encrypt_command(wrong_l_disable.data(),8u)!=Status::Ok)
     return fail("could not encrypt invalid KEY1 0x60 command");
 write_key1_command(wrong_l_disable);
 if(card.decode_next_key1_command(decoded_key1)!=Status::PermissionDenied)
     return fail("KEY1 0x60 accepted an llll value different from command 0x40");
 auto encrypted_disable_key2=make_key1_command(
     0x06u,UINT16_C(0x1234),packed_randoms,disable_counter);
 if(command_codec.encrypt_command(encrypted_disable_key2.data(),8u)!=Status::Ok)
     return fail("could not encrypt valid KEY1 0x60 command");
 write_key1_command(encrypted_disable_key2);
 if(card.decode_next_key1_command(decoded_key1)!=Status::Ok||
    decoded_key1.kind!=NdsGameCardKey1CommandKind::DisableKey2||
    card.complete_key1_command_gap(0u)!=Status::Ok)
     return fail("KEY1 0x60 rejected the original llll value or first gap");
 card.advance_bus_cycles(key1_secure_delay);
 if(card.decode_next_key1_command(decoded_key1)!=Status::Ok||
    card.complete_key1_command_gap(0u)!=Status::Ok)
     return fail("KEY1 0x60 second transmission/gap failed after secure delay");

 const uint32_t main_mode_counter=disable_counter+1u;
 auto encrypted_main_mode=make_key1_command(
     0x0Au,UINT16_C(0x1234),packed_randoms,main_mode_counter);
 if(command_codec.encrypt_command(encrypted_main_mode.data(),8u)!=Status::Ok)
     return fail("could not encrypt KEY1 0xA0 command");
 write_key1_command(encrypted_main_mode);
 if(card.decode_next_key1_command(decoded_key1)!=Status::Ok||
    decoded_key1.kind!=NdsGameCardKey1CommandKind::EnterMainDataMode||
    card.complete_key1_command_gap(0u)!=Status::Ok)
     return fail("KEY1 0xA0 did not validate llll or complete its first gap");
 card.advance_bus_cycles(key1_secure_delay);
 if(card.decode_next_key1_command(decoded_key1)!=Status::Ok||
    card.complete_key1_command_gap(0u)!=Status::Ok)
     return fail("KEY1 0xA0 second transmission/gap failed after secure delay");

 /* The old/smaller variant increments k on every command and rejects replay. */
 card.reset_transfer();
 if(card.key1_random_l_initialized()||card.key1_random_l()!=0u||
    card.set_chip_id(UINT32_C(0x000000C2))!=Status::Ok||
    card.load_key1_key_table(bios_key_table.data(),
        static_cast<uint32_t>(bios_key_table.size()))!=Status::Ok||
    card.newer_protocol_variant())
     return fail("could not configure old KEY1 protocol variant");
 bus_write_u16(NdsGameCardControl::AuxSpiControlAddress,
     uint16_t(NdsGameCardControl::AuxSpiIrqEnableMask|
              NdsGameCardControl::AuxSpiSlotEnableMask));
 write_activate_key1(key1_random_i,key1_random_j,key1_counter);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     no_reply_control|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok||
    !card.key1_command_mode())
     return fail("old variant KEY1 activation failed");
 constexpr uint32_t old_gap_control=no_reply_control|
     UINT32_C(0x8F8)|
     (UINT32_C(0x18)<<NdsGameCardControl::RomCtrlKey1Gap2Shift)|
     NdsGameCardControl::RomCtrlGapClockMask;
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     old_gap_control)!=Status::Ok)
     return fail("could not configure older-cart KEY1 gap pulses");
 auto old_variant_command=make_key1_command(
     0x04u,0u,UINT32_C(0x123456),key1_counter+1u);
 if(command_codec.encrypt_command(old_variant_command.data(),8u)!=Status::Ok)
     return fail("could not encrypt old-variant KEY1 command");
 write_key1_command(old_variant_command);
 if(card.decode_next_key1_command(decoded_key1)!=Status::Ok||
    card.complete_key1_command_gap(0u)!=Status::PermissionDenied)
     return fail("old cart accepted a zero-clock gap while ROMCTRL gap pulses were enabled");
 NdsGameCardKey2 expected_old_key2;
 std::array<uint8_t,UINT32_C(0x910)> expected_old_gap{};
 if(expected_old_key2.initialize_from_random_seed(UINT32_C(0x123456),3u)!=Status::Ok||
    expected_old_key2.apply_keystream(expected_old_gap.data(),
        static_cast<uint32_t>(expected_old_gap.size()))!=Status::Ok||
    card.complete_key1_command_gap(UINT32_C(0x910))!=Status::Ok||
    !card.key2_initialized()||
    card.key2_state1()!=expected_old_key2.state1()||
    card.key2_state2()!=expected_old_key2.state2()||
    card.key2_bytes_processed()!=UINT32_C(0x910)||
    card.decode_next_key1_command(decoded_key1)!=Status::PermissionDenied)
     return fail("old cart 0x40 did not initialize and advance KEY2 across the 0x910 clocked gap or accepted a replay");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)
     return fail("could not clear old variant KEY1 activation IRQ");
 if(!oracis_bus_unbind_game_card_control(&card)||!oracis_bus_unbind_dma_control(&dma)||
    !oracis_bus_unbind_interrupt_control(&irq)||!oracis_bus_unbind_memory(&memory)||
    !oracis_arm_runtime_unbind_cpus(&a9,&a7))return fail("bus teardown failed");
 std::puts("NDS Game Card dummy/header commands, KEY1 activation and command sequencing, B7 limits, DRQ, ARM9/ARM7 DMA and IRQ passed");
 return 0;
}
