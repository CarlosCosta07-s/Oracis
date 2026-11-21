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
 if(card.mount_image(image.data(),uint32_t(image.size()))!=Status::Ok)return fail("mount failed");
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
 bus_write_u16(NdsGameCardControl::SeedAddress+8u,static_cast<uint16_t>(arm9_raw_seed0>>32u));
 bus_write_u16(NdsGameCardControl::SeedAddress+10u,static_cast<uint16_t>(arm9_raw_seed1>>32u));
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
 bus_write_u16(NdsGameCardControl::SeedAddress+8u,static_cast<uint16_t>(arm7_raw_seed0>>32u));
 bus_write_u16(NdsGameCardControl::SeedAddress+10u,static_cast<uint16_t>(arm7_raw_seed1>>32u));
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
 write_b7(0x8000u);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,
     normal|NdsGameCardControl::RomCtrlKey2DataMask|
     NdsGameCardControl::RomCtrlStartBusyMask)!=Status::PermissionDenied)
     return fail("KEY2-encrypted read was incorrectly accepted");
 if(!oracis_bus_unbind_game_card_control(&card)||!oracis_bus_unbind_dma_control(&dma)||
    !oracis_bus_unbind_interrupt_control(&irq)||!oracis_bus_unbind_memory(&memory)||
    !oracis_arm_runtime_unbind_cpus(&a9,&a7))return fail("bus teardown failed");
 std::puts("NDS Game Card dummy/header commands, B7 limits, DRQ, ARM9/ARM7 DMA and IRQ passed");
 return 0;
}
