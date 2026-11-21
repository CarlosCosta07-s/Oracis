#include "oracis/memory/nds_game_card_control.hpp"
#include "oracis/memory/nds_dma_control.hpp"
namespace oracis::memory {
namespace {
constexpr uint32_t kWritable=UINT32_C(0x7F7F7FFF),kBlockMask=7u,kStartBytes=8u,kWordBytes=4u;
constexpr uint32_t kHeaderBytes=0x200u,kDummyBytes=0x2000u,kRetailWindowBytes=0x1000u,kRetailRedirectBase=0x8000u,kRetailRedirectMask=0x01FFu;
uint32_t block_bytes(uint32_t code) noexcept {if(code==0u)return 0u;if(code==7u)return 4u;return UINT32_C(0x100)<<code;}
}
bool NdsGameCardControl::valid_cpu(NdsCpu c) noexcept{return c==NDS_ARM9||c==NDS_ARM7;}
bool NdsGameCardControl::command_address(uint32_t a) noexcept{return a>=CommandAddress&&a<CommandAddress+CommandBytes;}
bool NdsGameCardControl::seed_address(uint32_t a) noexcept{return a>=SeedAddress&&a<SeedAddress+SeedBytes;}
Status NdsGameCardControl::install() noexcept{if(installed_)return Status::InvalidRegion;reset_transfer();command_={};seed_registers_={};aux_spi_control_=0u;installed_=true;return Status::Ok;}
Status NdsGameCardControl::mount_image(const uint8_t*b,uint32_t n) noexcept{
 if(!installed_)return Status::Unmapped;
 if(!b||n<UINT32_C(0x10))return Status::InvalidRegion;
 if(busy_)return Status::PermissionDenied;
 image_=b;image_size_=n;chip_id_=0u;chip_id_configured_=false;
 header_protocol_available_=n>=(HeaderSecureAreaDelayOffset+2u);
 header_normal_rom_control_=0u;header_key1_rom_control_=0u;
 header_secure_area_delay_=0u;key2_seed_index_=0u;
 if(n>HeaderKey2SeedIndexOffset)key2_seed_index_=static_cast<uint8_t>(b[HeaderKey2SeedIndexOffset]&UINT8_C(0x07));
 if(header_protocol_available_){
  header_normal_rom_control_=static_cast<uint32_t>(b[HeaderNormalRomControlOffset])|
      (static_cast<uint32_t>(b[HeaderNormalRomControlOffset+1u])<<8u)|
      (static_cast<uint32_t>(b[HeaderNormalRomControlOffset+2u])<<16u)|
      (static_cast<uint32_t>(b[HeaderNormalRomControlOffset+3u])<<24u);
  header_key1_rom_control_=static_cast<uint32_t>(b[HeaderKey1RomControlOffset])|
      (static_cast<uint32_t>(b[HeaderKey1RomControlOffset+1u])<<8u)|
      (static_cast<uint32_t>(b[HeaderKey1RomControlOffset+2u])<<16u)|
      (static_cast<uint32_t>(b[HeaderKey1RomControlOffset+3u])<<24u);
  header_secure_area_delay_=static_cast<uint16_t>(
      static_cast<uint16_t>(b[HeaderSecureAreaDelayOffset])|
      (static_cast<uint16_t>(b[HeaderSecureAreaDelayOffset+1u])<<8u));
 }
 key2_.clear();key1_.reset();
 key1_command_mode_=false;key1_activation_pending_=false;
 key1_random_i_=0u;key1_random_j_=0u;key1_command_counter_=0u;
 last_key1_command_={};last_key1_command_valid_=false;last_key1_command_transmissions_=0u;
 last_key1_command_opcode_=0u;key1_repeat_delay_cycles_remaining_=0u;
 if(key1_.table_loaded()){
  const Status key_status=initialize_key1_from_image();
  if(key_status!=Status::Ok){image_=nullptr;image_size_=0u;key1_.reset();return key_status;}
 }
 return Status::Ok;
}
Status NdsGameCardControl::load_key1_key_table(const uint8_t*b,uint32_t n) noexcept{
 if(!installed_)return Status::Unmapped;
 if(busy_||key1_command_mode_)return Status::PermissionDenied;
 const Status status=key1_.load_key_table(b,n);
 if(status!=Status::Ok)return status;
 return image_==nullptr?Status::Ok:initialize_key1_from_image();
}
Status NdsGameCardControl::initialize_key1_from_image() noexcept{
 if(image_==nullptr)return Status::Unmapped;
 if(image_size_<UINT32_C(0x10))return Status::InvalidRegion;
 const uint32_t game_code=
  static_cast<uint32_t>(image_[0x0Cu])|
  (static_cast<uint32_t>(image_[0x0Du])<<8u)|
  (static_cast<uint32_t>(image_[0x0Eu])<<16u)|
  (static_cast<uint32_t>(image_[0x0Fu])<<24u);
 return key1_.initialize(game_code,2u,8u);
}
Status NdsGameCardControl::set_chip_id(uint32_t id) noexcept{if(!installed_)return Status::Unmapped;if(!image_)return Status::PermissionDenied;if(busy_||key1_command_mode_)return Status::PermissionDenied;chip_id_=id;chip_id_configured_=true;return Status::Ok;}
void NdsGameCardControl::unmount_image() noexcept{reset_transfer();image_=nullptr;image_size_=0u;chip_id_=0u;chip_id_configured_=false;header_protocol_available_=false;header_normal_rom_control_=0u;header_key1_rom_control_=0u;header_secure_area_delay_=0u;key2_seed_index_=0u;}
void NdsGameCardControl::reset_transfer() noexcept{command_={};seed_registers_={};key1_.reset();key2_.clear();aux_spi_control_=0u;rom_control_=0u;current_offset_=0u;current_window_base_=0u;remaining_words_=0u;cycles_until_data_=0u;cycles_per_byte_=5u;owner_cpu_=NDS_ARM9;reset_released_=false;busy_=false;data_ready_=false;dummy_data_=false;chip_id_data_=false;wrap_read_window_=false;key2_data_active_=false;key1_command_mode_=false;key1_activation_pending_=false;key1_random_i_=0u;key1_random_j_=0u;key1_command_counter_=0u;last_key1_command_={};last_key1_command_valid_=false;last_key1_command_transmissions_=0u;last_key1_command_opcode_=0u;key1_repeat_delay_cycles_remaining_=0u;}
Status NdsGameCardControl::decode_next_key1_command(
    NdsGameCardKey1Command &decoded) noexcept
{
 if(!installed_)return Status::Unmapped;
 if(!key1_command_mode_||busy_||!chip_id_configured_)
  return Status::PermissionDenied;
 const bool newer=newer_protocol_variant();
 if(newer&&!header_protocol_available_)return Status::PermissionDenied;
 const uint8_t *wire_bytes=command_.data();

 bool repeated=false;
 if(newer&&last_key1_command_valid_){
  repeated=true;
  for(uint32_t index=0u;index<CommandBytes;++index){
   if(wire_bytes[index]!=last_key1_command_[index]){
    repeated=false;
    break;
   }
  }
 }
 if(repeated&&key1_repeat_delay_cycles_remaining_!=0u)
  return Status::PermissionDenied;
 if(!repeated&&newer&&last_key1_command_valid_){
  const uint8_t previous_maximum=
      last_key1_command_opcode_==UINT8_C(0x02)?UINT8_C(9):UINT8_C(2);
  if(last_key1_command_transmissions_<previous_maximum)
   return Status::PermissionDenied;
 }

 const uint32_t expected_counter=repeated
     ? key1_command_counter_
     : ((key1_command_counter_+1u)&UINT32_C(0x000FFFFF));
 NdsGameCardKey1Command candidate{};
 const Status status=key1_.decode_command(
     wire_bytes,CommandBytes,expected_counter,
     key1_random_i_,key1_random_j_,candidate);
 if(status!=Status::Ok)return status;

 if(repeated){
  const uint8_t maximum_transmissions=
      candidate.kind==NdsGameCardKey1CommandKind::ReadSecureAreaBlock
          ? UINT8_C(9) : UINT8_C(2);
  if(last_key1_command_transmissions_>=maximum_transmissions)
   return Status::PermissionDenied;
  ++last_key1_command_transmissions_;
  key1_repeat_delay_cycles_remaining_=0u;
 }else{
  key1_command_counter_=candidate.counter20;
  last_key1_command_transmissions_=UINT8_C(1);
  last_key1_command_opcode_=candidate.opcode;
  key1_repeat_delay_cycles_remaining_=newer
      ? header_secure_area_delay_core_cycles() : 0u;
 }
 for(uint32_t index=0u;index<CommandBytes;++index)
  last_key1_command_[index]=wire_bytes[index];
 last_key1_command_valid_=true;
 decoded=candidate;
 return Status::Ok;
}
uint32_t NdsGameCardControl::read_rom_control() const noexcept{uint32_t v=rom_control_;if(data_ready_)v|=RomCtrlDrqMask;if(busy_)v|=RomCtrlStartBusyMask;return v;}
Status NdsGameCardControl::start_transfer(NdsCpu cpu) noexcept{
 if(!installed_)return Status::Unmapped;if(!valid_cpu(cpu))return Status::InvalidCpu;
 if(busy_||!reset_released_||!image_)return Status::PermissionDenied;
 if((aux_spi_control_&AuxSpiSlotEnableMask)==0u ||
    (aux_spi_control_&AuxSpiSlotModeMask)!=0u)return Status::PermissionDenied;
 if((rom_control_&(RomCtrlKey2CommandMask|RomCtrlWriteMask))!=0u)return Status::PermissionDenied;
 if(key1_command_mode_)return Status::PermissionDenied;
 if((rom_control_&RomCtrlKey2DataMask)!=0u&&!key2_.initialized())return Status::PermissionDenied;

 const uint32_t block_code=(rom_control_>>RomCtrlBlockSizeShift)&kBlockMask;
 const uint32_t size=block_bytes(block_code);
 uint32_t offset=0u;
 bool dummy=false;
 bool wrap_window=false;
 bool activate_key1=false;
 if(command_[0]==ActivateKey1Command){
  /* 3C has no defined reply length. Some hosts may still clock one
   * high-Z word, so retain that explicit four-byte diagnostic form. */
  if(!key1_.initialized()||(block_code!=0u&&block_code!=7u)||
     (rom_control_&RomCtrlKey2DataMask)!=0u)return Status::PermissionDenied;
  dummy=(size!=0u);
  activate_key1=true;
 }else if(size==0u||(size%kWordBytes)!=0u){
  return Status::PermissionDenied;
 }else if(command_[0]==DummyCommand){
  /* After reset the 9F dummy command clocks out an 8 KiB high-Z stream. */
  if(size!=kDummyBytes)return Status::PermissionDenied;
  dummy=true;
 }else if(command_[0]==HeaderReadCommand){
  /* The initial 00 command returns the raw 0x200-byte NDS header. */
  if(size!=kHeaderBytes)return Status::PermissionDenied;
 }else if(command_[0]==ChipIdReadCommand){
  /* The first 90 command returns the caller-configured raw 32-bit chip ID. */
  if(!chip_id_configured_)return Status::PermissionDenied;
  chip_id_data_=true;
 }else if(command_[0]==RomReadCommand){
  const uint32_t requested_offset =
      (uint32_t(command_[1])<<24u)|(uint32_t(command_[2])<<16u)|
      (uint32_t(command_[3])<<8u)|uint32_t(command_[4]);
  /*
   * Retail cards redirect 0000-7FFF reads into the main data area. B7 is
   * byte-addressed and wraps to the start of its active 4 KiB window.
   */
  offset=requested_offset<kRetailRedirectBase
      ? kRetailRedirectBase+(requested_offset&kRetailRedirectMask)
      : requested_offset;
  wrap_window=true;
 }else{
  return Status::PermissionDenied;
 }

 const uint32_t window_base = wrap_window
     ? (offset & ~(kRetailWindowBytes-1u)) : 0u;
 if(!dummy&&!chip_id_data_){
  /* Preflight every byte of the requested stream, including 4 KiB wrap. */
  for(uint32_t byte=0u;byte<size;++byte){
   const uint64_t address=wrap_window
       ? static_cast<uint64_t>(window_base)|
           ((offset-window_base+byte)&(kRetailWindowBytes-1u))
       : static_cast<uint64_t>(offset)+byte;
   if(address>=image_size_)return Status::AddressOverflow;
  }
 }

 current_offset_=offset;
 current_window_base_=window_base;
 remaining_words_=size/kWordBytes;
 if(activate_key1){
  key1_random_i_=static_cast<uint16_t>((static_cast<uint16_t>(command_[1])<<4u)|
      (static_cast<uint16_t>(command_[2])>>4u));
  key1_random_j_=static_cast<uint16_t>(
      (static_cast<uint16_t>(command_[2]&UINT8_C(0x0F))<<8u)|command_[3]);
  /* xkkkkkxx: low nibble of byte 4, then bytes 5 and 6. */
  key1_command_counter_=
      (static_cast<uint32_t>(command_[4]&UINT8_C(0x0F))<<16u)|
      (static_cast<uint32_t>(command_[5])<<8u)|command_[6];
 }
 key1_activation_pending_=activate_key1;
 dummy_data_=dummy;
 wrap_read_window_=wrap_window;
 cycles_per_byte_=(rom_control_&RomCtrlClockSlowMask)!=0u?8u:5u;
 cycles_until_data_=static_cast<uint64_t>(kStartBytes)*cycles_per_byte_;
 owner_cpu_=cpu;
 key2_data_active_=wrap_window&&(rom_control_&RomCtrlKey2DataMask)!=0u;
 busy_=true;
 data_ready_=false;
 /* A zero-length 3C command completes without waiting for a DATA word.
  * Its parameters remain in force for the following KEY1 commands; k is
  * not incremented by the activation command itself. */
 if(activate_key1&&size==0u)finish_transfer();
 return Status::Ok;
}
Status NdsGameCardControl::write_rom_control(NdsCpu cpu,uint32_t value) noexcept{
 if(busy_&&(value&RomCtrlStartBusyMask))return Status::PermissionDenied;
 const bool release=(value&RomCtrlResetReleaseMask)!=0u;
 rom_control_=value&kWritable;
 if(release)reset_released_=true;
 if(reset_released_)rom_control_|=RomCtrlResetReleaseMask;

 if((value&RomCtrlKey2SeedApplyMask)!=0u){
  const auto &bank=seed_registers_[cpu==NDS_ARM9?0u:1u];
  const uint64_t raw_seed1=
      static_cast<uint64_t>(bank[0])|
      (static_cast<uint64_t>(bank[1])<<8u)|
      (static_cast<uint64_t>(bank[2])<<16u)|
      (static_cast<uint64_t>(bank[3])<<24u)|
      ((static_cast<uint64_t>(bank[8])&UINT64_C(0x7F))<<32u);
  const uint64_t raw_seed2=
      static_cast<uint64_t>(bank[4])|
      (static_cast<uint64_t>(bank[5])<<8u)|
      (static_cast<uint64_t>(bank[6])<<16u)|
      (static_cast<uint64_t>(bank[7])<<24u)|
      ((static_cast<uint64_t>(bank[10])&UINT64_C(0x7F))<<32u);
  const Status seed_status=key2_.set_raw_seeds(raw_seed1,raw_seed2);
  if(seed_status!=Status::Ok)return seed_status;
 }

 if((value&RomCtrlStartBusyMask)==0u)return Status::Ok;
 return start_transfer(cpu);
}
ReadResult NdsGameCardControl::read_data(NdsCpu cpu) noexcept{
 if(!installed_)return{Status::Unmapped,0u};if(!valid_cpu(cpu))return{Status::InvalidCpu,0u};
 if(cpu!=owner_cpu_)return{Status::CpuAccessDenied,0u};
 if(!busy_||!data_ready_||remaining_words_==0u)return{Status::PermissionDenied,0u};
 uint8_t bytes[kWordBytes]{};
 if(dummy_data_){
  for(uint32_t byte=0u;byte<kWordBytes;++byte)bytes[byte]=UINT8_MAX;
 }else if(chip_id_data_){
  for(uint32_t byte=0u;byte<kWordBytes;++byte)
   bytes[byte]=static_cast<uint8_t>(chip_id_>>(byte*8u));
 }else{
  for(uint32_t byte=0u;byte<kWordBytes;++byte){
   const uint32_t address=wrap_read_window_
       ? current_window_base_|
           ((current_offset_-current_window_base_+byte)&(kRetailWindowBytes-1u))
       : current_offset_+byte;
   bytes[byte]=image_[address];
  }
  current_offset_=wrap_read_window_
      ? current_window_base_|
          ((current_offset_-current_window_base_+kWordBytes)&
              (kRetailWindowBytes-1u))
      : current_offset_+kWordBytes;
 }
 if(key2_data_active_){
  const Status key_status=key2_.apply_keystream(bytes,kWordBytes);
  if(key_status!=Status::Ok)return{key_status,0u};
 }
 const uint32_t value=static_cast<uint32_t>(bytes[0])|
     (static_cast<uint32_t>(bytes[1])<<8u)|
     (static_cast<uint32_t>(bytes[2])<<16u)|
     (static_cast<uint32_t>(bytes[3])<<24u);
 --remaining_words_;data_ready_=false;
 if(remaining_words_==0u)finish_transfer();
 else cycles_until_data_=static_cast<uint64_t>(kWordBytes)*cycles_per_byte_;
 return{Status::Ok,value};
}
void NdsGameCardControl::finish_transfer() noexcept{busy_=false;data_ready_=false;dummy_data_=false;chip_id_data_=false;wrap_read_window_=false;key2_data_active_=false;current_window_base_=0u;cycles_until_data_=0u;if(key1_activation_pending_){key1_command_mode_=true;key1_activation_pending_=false;}if(aux_spi_control_&AuxSpiIrqEnableMask)(void)interrupts_.request_irq(owner_cpu_,GameCardTransferIrqMask);}
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
 if(seed_address(a)){if(busy_)return Status::PermissionDenied;const uint32_t o=a-SeedAddress;const bool low=o<8u&&w==4u&&(o&3u)==0u;const bool high=o>=8u&&w==2u&&(o&1u)==0u;if(!low&&!high)return Status::InvalidWidth;auto &bank=seed_registers_[cpu==NDS_ARM9?0u:1u];for(uint32_t i=0;i<w;++i)bank[o+i]=uint8_t(v>>(i*8u));return Status::Ok;}
 if(a==DataAddress)return w==4u?Status::PermissionDenied:Status::InvalidWidth;
 return Status::Unmapped;
}
}
