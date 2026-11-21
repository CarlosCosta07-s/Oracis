#include "oracis/memory/nds_game_card_control.hpp"
#include "oracis/memory/nds_dma_control.hpp"
namespace oracis::memory {
namespace {
constexpr uint32_t kWritable=UINT32_C(0x7F7F7FFF),kBlockMask=7u,kStartBytes=8u,kWordBytes=4u,kReadCommand=0xB7u;
uint32_t block_bytes(uint32_t code) noexcept {if(code==0u)return 0u;if(code==7u)return 4u;return UINT32_C(0x100)<<code;}
}
bool NdsGameCardControl::valid_cpu(NdsCpu c) noexcept{return c==NDS_ARM9||c==NDS_ARM7;}
bool NdsGameCardControl::command_address(uint32_t a) noexcept{return a>=CommandAddress&&a<CommandAddress+CommandBytes;}
bool NdsGameCardControl::seed_address(uint32_t a) noexcept{return a>=SeedAddress&&a<SeedAddress+SeedBytes;}
Status NdsGameCardControl::install() noexcept{if(installed_)return Status::InvalidRegion;reset_transfer();command_={};seed_registers_={};aux_spi_control_=0u;installed_=true;return Status::Ok;}
Status NdsGameCardControl::mount_image(const uint8_t*b,uint32_t n) noexcept{if(!installed_)return Status::Unmapped;if(!b||n==0u)return Status::InvalidRegion;if(busy_)return Status::PermissionDenied;image_=b;image_size_=n;return Status::Ok;}
void NdsGameCardControl::unmount_image() noexcept{reset_transfer();image_=nullptr;image_size_=0u;}
void NdsGameCardControl::reset_transfer() noexcept{command_={};seed_registers_={};aux_spi_control_=0u;rom_control_=0u;current_offset_=0u;remaining_words_=0u;cycles_until_data_=0u;cycles_per_byte_=5u;owner_cpu_=NDS_ARM9;reset_released_=false;busy_=false;data_ready_=false;}
uint32_t NdsGameCardControl::read_rom_control() const noexcept{uint32_t v=rom_control_;if(data_ready_)v|=RomCtrlDrqMask;if(busy_)v|=RomCtrlStartBusyMask;return v;}
Status NdsGameCardControl::start_transfer(NdsCpu cpu) noexcept{
 if(!installed_)return Status::Unmapped;if(!valid_cpu(cpu))return Status::InvalidCpu;
 if(busy_||!reset_released_||!image_)return Status::PermissionDenied;
 if((aux_spi_control_&AuxSpiSlotEnableMask)==0u ||
    (aux_spi_control_&AuxSpiSlotModeMask)!=0u)return Status::PermissionDenied;
 if((rom_control_&(RomCtrlKey2DataMask|RomCtrlKey2CommandMask|RomCtrlWriteMask))!=0u)return Status::PermissionDenied;
 if(command_[0]!=kReadCommand)return Status::PermissionDenied;
 const uint32_t size=block_bytes((rom_control_>>RomCtrlBlockSizeShift)&kBlockMask);
 if(size==0u||(size%kWordBytes)!=0u)return Status::PermissionDenied;
 const uint32_t offset=(uint32_t(command_[1])<<24u)|(uint32_t(command_[2])<<16u)|(uint32_t(command_[3])<<8u)|uint32_t(command_[4]);
 if(uint64_t(offset)+size>image_size_)return Status::AddressOverflow;
 current_offset_=offset;remaining_words_=size/kWordBytes;
 cycles_per_byte_=(rom_control_&RomCtrlClockSlowMask)?8u:5u;
 cycles_until_data_=uint64_t(kStartBytes)*cycles_per_byte_;owner_cpu_=cpu;busy_=true;data_ready_=false;return Status::Ok;
}
Status NdsGameCardControl::write_rom_control(NdsCpu cpu,uint32_t value) noexcept{
 if(busy_&&(value&RomCtrlStartBusyMask))return Status::PermissionDenied;
 const bool release=(value&RomCtrlResetReleaseMask)!=0u;
 rom_control_=value&kWritable;
 if(release)reset_released_=true;
 if(reset_released_)rom_control_|=RomCtrlResetReleaseMask;
 if((value&RomCtrlStartBusyMask)==0u)return Status::Ok;
 return start_transfer(cpu);
}
ReadResult NdsGameCardControl::read_data(NdsCpu cpu) noexcept{
 if(!installed_)return{Status::Unmapped,0u};if(!valid_cpu(cpu))return{Status::InvalidCpu,0u};
 if(cpu!=owner_cpu_)return{Status::CpuAccessDenied,0u};
 if(!busy_||!data_ready_||remaining_words_==0u)return{Status::PermissionDenied,0u};
 const uint32_t v=uint32_t(image_[current_offset_])|(uint32_t(image_[current_offset_+1u])<<8u)|(uint32_t(image_[current_offset_+2u])<<16u)|(uint32_t(image_[current_offset_+3u])<<24u);
 current_offset_+=kWordBytes;--remaining_words_;data_ready_=false;
 if(remaining_words_==0u)finish_transfer();else cycles_until_data_=uint64_t(kWordBytes)*cycles_per_byte_;
 return{Status::Ok,v};
}
void NdsGameCardControl::finish_transfer() noexcept{busy_=false;data_ready_=false;cycles_until_data_=0u;if(aux_spi_control_&AuxSpiIrqEnableMask)(void)interrupts_.request_irq(owner_cpu_,GameCardTransferIrqMask);}
void NdsGameCardControl::advance_bus_cycles(uint64_t cycles) noexcept{
 if(!installed_||cycles==0u)return;
 while(busy_&&!data_ready_){
  if(cycles<cycles_until_data_){cycles_until_data_-=cycles;return;}
  cycles-=cycles_until_data_;cycles_until_data_=0u;data_ready_=true;
  if(dma_control_!=nullptr)(void)dma_control_->trigger_game_card(owner_cpu_);
  /* If DMA did not consume the word, DRQ remains visible to the guest CPU. */
  if(data_ready_)return;
  if(cycles==0u)return;
 }
}
ReadResult NdsGameCardControl::read(NdsCpu cpu,uint32_t a,uint32_t w) noexcept{
 if(!installed_)return{Status::Unmapped,0u};if(!valid_cpu(cpu))return{Status::InvalidCpu,0u};
 if(a==AuxSpiControlAddress){if(w!=2u)return{Status::InvalidWidth,0u};return{Status::Ok,aux_spi_control_};}
 if(a==RomControlAddress){if(w!=4u)return{Status::InvalidWidth,0u};return{Status::Ok,read_rom_control()};}
 if(command_address(a))return{Status::PermissionDenied,0u};
 if(seed_address(a)){const uint32_t o=a-SeedAddress;const bool low=o<8u&&w==4u&&(o&3u)==0u;const bool high=o>=8u&&w==2u&&(o&1u)==0u;if(!low&&!high)return{Status::InvalidWidth,0u};return{Status::PermissionDenied,0u};}
 if(a==DataAddress){if(w!=4u)return{Status::InvalidWidth,0u};return read_data(cpu);}
 return{Status::Unmapped,0u};
}
Status NdsGameCardControl::write(NdsCpu cpu,uint32_t a,uint32_t w,uint32_t v) noexcept{
 if(!installed_)return Status::Unmapped;if(!valid_cpu(cpu))return Status::InvalidCpu;
 if(a==AuxSpiControlAddress){if(w!=2u)return Status::InvalidWidth;aux_spi_control_=uint16_t(v)&UINT16_C(0xE07F);return Status::Ok;}
 if(a==RomControlAddress){if(w!=4u)return Status::InvalidWidth;return write_rom_control(cpu,v);}
 if(command_address(a)){if(busy_)return Status::PermissionDenied;const uint32_t o=a-CommandAddress;if((w!=1u&&w!=2u&&w!=4u)||o+w>CommandBytes)return Status::InvalidWidth;for(uint32_t i=0;i<w;++i)command_[o+i]=uint8_t(v>>(i*8u));return Status::Ok;}
 if(seed_address(a)){if(busy_)return Status::PermissionDenied;const uint32_t o=a-SeedAddress;const bool low=o<8u&&w==4u&&(o&3u)==0u;const bool high=o>=8u&&w==2u&&(o&1u)==0u;if(!low&&!high)return Status::InvalidWidth;for(uint32_t i=0;i<w;++i)seed_registers_[o+i]=uint8_t(v>>(i*8u));return Status::Ok;}
 if(a==DataAddress)return w==4u?Status::PermissionDenied:Status::InvalidWidth;
 return Status::Unmapped;
}
}
