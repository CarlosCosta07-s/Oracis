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
void write_b7(uint32_t off){using oracis::memory::NdsGameCardControl;const uint8_t b[8]={0xB7u,uint8_t(off>>24u),uint8_t(off>>16u),uint8_t(off>>8u),uint8_t(off),0u,0u,0u};for(uint32_t i=0;i<8u;++i)bus_write_u8(NdsGameCardControl::CommandAddress+i,b[i]);}
}
int main(){
 using namespace oracis::memory;
 GuestMemory memory;
 if(memory.map_ram(0x02000000u,0x1000u,Readable|Writable,"game-card-dma-test")!=Status::Ok)return fail("RAM map failed");
 NdsInterruptControl irq(memory);NdsGameCardControl card(irq);NdsDmaControl dma(memory,irq);
 if(card.install()!=Status::Ok)return fail("install failed");
 dma.attach_game_card_control(card);card.attach_dma_control(dma);
 std::array<uint8_t,0x400> image{};for(uint32_t i=0;i<image.size();++i)image[i]=uint8_t((i*13u+7u)&255u);
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
 write_b7(0x20u);
 const uint32_t small=NdsGameCardControl::RomCtrlResetReleaseMask|(7u<<NdsGameCardControl::RomCtrlBlockSizeShift);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,small|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok||!card.busy())return fail("B7 transaction failed to start");
 if(bus_read_u32(NdsGameCardControl::RomControlAddress)&NdsGameCardControl::RomCtrlDrqMask)return fail("early DRQ");
 card.advance_bus_cycles(39u);
 if(bus_read_u32(NdsGameCardControl::RomControlAddress)&NdsGameCardControl::RomCtrlDrqMask)return fail("first word latency wrong");
 card.advance_bus_cycles(1u);
 if(!(bus_read_u32(NdsGameCardControl::RomControlAddress)&NdsGameCardControl::RomCtrlDrqMask))return fail("DRQ not set");
 if(bus_read_u32(NdsGameCardControl::DataAddress)!=word(0x20u)||card.busy()||
    ((bus_read_u32(NdsGameCardControl::RomControlAddress)&(NdsGameCardControl::RomCtrlStartBusyMask|NdsGameCardControl::RomCtrlDrqMask))!=0u)||
    !(bus_read_u32(NdsInterruptControl::IfAddress)&imask))return fail("one-word read or completion IRQ failed");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)return fail("clear irq failed");
 write_b7(0x100u);
 const uint32_t normal=NdsGameCardControl::RomCtrlResetReleaseMask|(1u<<NdsGameCardControl::RomCtrlBlockSizeShift);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,normal|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok)return fail("block read start failed");
 card.advance_bus_cycles(40u);
 for(uint32_t n=0;n<128u;++n){if(n&&!(bus_read_u32(NdsGameCardControl::RomControlAddress)&NdsGameCardControl::RomCtrlDrqMask))return fail("DRQ not ready for word");if(bus_read_u32(NdsGameCardControl::DataAddress)!=word(0x100u+n*4u))return fail("block word mismatch");if(n<127u)card.advance_bus_cycles(20u);}
 if(card.busy()||!(bus_read_u32(NdsInterruptControl::IfAddress)&imask))return fail("128 word transfer incomplete");
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)return fail("second irq clear failed");
 /* ARM9 Game Card DMA timing 5; one DRQ edge transfers one fixed-source word. */
 if(irq.write(NDS_ARM9,NdsInterruptControl::IfAddress,4u,imask)!=Status::Ok)return fail("clear card IRQ before DMA failed");
 constexpr uint32_t dma0=NdsDmaControl::FirstRegisterAddress,dma1=dma0+12u;
 write_b7(0x40u);bus_write_u32(dma0,NdsGameCardControl::DataAddress);bus_write_u32(dma0+4u,0x02000000u);
 bus_write_u32(dma0+8u,1u|(2u<<NdsDmaControl::SourceControlShift)|(5u<<27u)|NdsDmaControl::RepeatMask|NdsDmaControl::WordTransferMask|NdsDmaControl::IrqEnableMask|NdsDmaControl::EnableMask);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,small|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok)return fail("ARM9 DMA card command failed");
 card.advance_bus_cycles(40u);
 if(memory.read(NDS_ARM9,0x02000000u,4u).value!=word(0x40u)||card.busy()||
    (bus_read_u32(dma0+8u)&NdsDmaControl::EnableMask)==0u||
    (irq.read(NDS_ARM9,NdsInterruptControl::IfAddress,4u).value&((1u<<8u)|imask))!=((1u<<8u)|imask))
    return fail("ARM9 DMA did not consume Game Card DRQ");
 bus_write_u16(dma0+10u,0u);if(card.busy())return fail("ARM9 one-word transfer did not complete");
 /* ARM7 Game Card DMA timing 2 uses its independent DMA register bank. */
 if(!oracis_arm_runtime_select_cpu(NDS_ARM7))return fail("ARM7 select failed");
 write_b7(0x44u);bus_write_u32(dma1,NdsGameCardControl::DataAddress);bus_write_u32(dma1+4u,0x02000004u);
 bus_write_u32(dma1+8u,1u|(2u<<NdsDmaControl::SourceControlShift)|(2u<<28u)|NdsDmaControl::RepeatMask|NdsDmaControl::WordTransferMask|NdsDmaControl::IrqEnableMask|NdsDmaControl::EnableMask);
 if(card.write(NDS_ARM7,NdsGameCardControl::RomControlAddress,4u,small|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::Ok)return fail("ARM7 DMA card command failed");
 card.advance_bus_cycles(40u);
 if(memory.read(NDS_ARM7,0x02000004u,4u).value!=word(0x44u)||card.busy()||
    (bus_read_u32(dma1+8u)&NdsDmaControl::EnableMask)==0u||
    (irq.read(NDS_ARM7,NdsInterruptControl::IfAddress,4u).value&((1u<<9u)|imask))!=((1u<<9u)|imask))
    return fail("ARM7 DMA did not consume Game Card DRQ");
 bus_write_u16(dma1+10u,0u);if(card.busy())return fail("ARM7 one-word transfer did not complete");
 if(!oracis_arm_runtime_select_cpu(NDS_ARM9))return fail("ARM9 restore failed");
 write_b7(0x400u);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,normal|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::AddressOverflow||card.busy())return fail("range check not transactional");
 write_b7(0u);
 if(card.write(NDS_ARM9,NdsGameCardControl::RomControlAddress,4u,normal|NdsGameCardControl::RomCtrlKey2DataMask|NdsGameCardControl::RomCtrlStartBusyMask)!=Status::PermissionDenied)return fail("KEY2 accepted unexpectedly");
 if(!oracis_bus_unbind_game_card_control(&card)||!oracis_bus_unbind_dma_control(&dma)||
    !oracis_bus_unbind_interrupt_control(&irq)||!oracis_bus_unbind_memory(&memory)||
    !oracis_arm_runtime_unbind_cpus(&a9,&a7))return fail("bus teardown failed");
 std::puts("NDS Game Card B7 reads, DRQ timing, ARM9/ARM7 DMA and completion IRQ passed");
 return 0;
}
