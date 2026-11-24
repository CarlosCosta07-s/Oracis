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
 key2_command_mode_=false;key1_main_data_mode_pending_=false;
 key2_command_transfer_active_=false;key2_transfer_data_bytes_=0u;key2_transfer_gap2_bytes_=0u;
 key1_random_i_=0u;key1_random_j_=0u;key1_random_l_=0u;key1_random_l_initialized_=false;key1_command_counter_=0u;
 last_key1_command_={};last_key1_command_valid_=false;last_key1_command_transmissions_=0u;
 last_key1_command_opcode_=0u;key1_repeat_delay_cycles_remaining_=0u;
 key1_key2_pending_seed24_=0u;key1_key2_activation_pending_=false;
 key1_command_gap_pending_=false;
 key1_gap_transfer_active_=false;key1_command_bytes_pending_=false;
 key1_gap_clock_bytes_=0u;key1_gap_cycles_=0u;
 key1_reply_payload_bytes_=0u;key1_reply_payload_encrypted_=false;
 key1_reply_data_offset_=0u;key1_reply_post_payload_zero_bytes_=0u;
 key1_post_payload_zero_pending_=false;key1_post_payload_zero_cycles_=0u;
 if(key1_.table_loaded()){
  const Status key_status=initialize_key1_from_image();
  if(key_status!=Status::Ok){image_=nullptr;image_size_=0u;key1_.reset();return key_status;}
 }
 return Status::Ok;
}
Status NdsGameCardControl::load_key1_key_table(const uint8_t*b,uint32_t n) noexcept{
 if(!installed_)return Status::Unmapped;
 if(busy_||key1_command_mode_||key2_command_mode_)return Status::PermissionDenied;
 const Status status=key1_.load_key_table(b,n);
 if(status!=Status::Ok)return status;
 return image_==nullptr?Status::Ok:initialize_key1_from_image();
}
Status NdsGameCardControl::decrypt_secure_area_prefix(
    uint8_t *bytes,uint32_t size,bool &identifier_valid) noexcept
{
 if(!installed_)return Status::Unmapped;
 identifier_valid=false;
 if(bytes==nullptr)return Status::InvalidRegion;
 if(size!=NdsGameCardKey1::SecureAreaEncryptedBytes)
  return Status::InvalidWidth;
 if(image_==nullptr)return Status::PermissionDenied;
 if(busy_||key1_command_mode_||key2_command_mode_)
  return Status::PermissionDenied;
 const uint64_t secure_area_end=
     static_cast<uint64_t>(SecureAreaDataOffset)+size;
 if(secure_area_end>image_size_)return Status::AddressOverflow;
 if(!key1_.initialized())return Status::PermissionDenied;

 /* Work on a caller-owned copy: the mounted ROM image stays byte-identical. */
 for(uint32_t index=0u;index<size;++index)
  bytes[index]=image_[SecureAreaDataOffset+index];
 return key1_.decrypt_secure_area(bytes,size,identifier_valid);
}
Status NdsGameCardControl::decrypt_firmware_boot_code(
    const uint8_t *firmware,
    std::size_t firmware_size,
    NdsFirmwareBootCode &output) noexcept
{
 if(!installed_)return Status::Unmapped;
 if(firmware==nullptr||firmware_size==0u)return Status::InvalidRegion;
 if(busy_||key1_command_mode_||key2_command_mode_)
  return Status::PermissionDenied;
 if(!key1_.table_loaded())return Status::PermissionDenied;
 return key1_.decrypt_firmware_boot_code(
     firmware,firmware_size,output);
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
Status NdsGameCardControl::set_chip_id(uint32_t id) noexcept{if(!installed_)return Status::Unmapped;if(!image_)return Status::PermissionDenied;if(busy_||key1_command_mode_||key2_command_mode_)return Status::PermissionDenied;chip_id_=id;chip_id_configured_=true;return Status::Ok;}
void NdsGameCardControl::unmount_image() noexcept{reset_transfer();image_=nullptr;image_size_=0u;chip_id_=0u;chip_id_configured_=false;header_protocol_available_=false;header_normal_rom_control_=0u;header_key1_rom_control_=0u;header_secure_area_delay_=0u;key2_seed_index_=0u;}
void NdsGameCardControl::reset_transfer() noexcept{command_={};seed_registers_={};key1_.reset();key2_.clear();aux_spi_control_=0u;rom_control_=0u;current_offset_=0u;current_window_base_=0u;remaining_words_=0u;cycles_until_data_=0u;cycles_per_byte_=5u;owner_cpu_=NDS_ARM9;reset_released_=false;busy_=false;data_ready_=false;dummy_data_=false;chip_id_data_=false;wrap_read_window_=false;key2_data_active_=false;key1_command_mode_=false;key1_activation_pending_=false;key2_command_mode_=false;key1_main_data_mode_pending_=false;key2_command_transfer_active_=false;key2_transfer_data_bytes_=0u;key2_transfer_gap2_bytes_=0u;key1_random_i_=0u;key1_random_j_=0u;key1_random_l_=0u;key1_random_l_initialized_=false;key1_command_counter_=0u;last_key1_command_={};last_key1_command_valid_=false;last_key1_command_transmissions_=0u;last_key1_command_opcode_=0u;key1_repeat_delay_cycles_remaining_=0u;key1_key2_pending_seed24_=0u;key1_key2_activation_pending_=false;key1_command_gap_pending_=false;key1_gap_transfer_active_=false;key1_command_bytes_pending_=false;key1_gap_clock_bytes_=0u;key1_gap_cycles_=0u;key1_reply_payload_bytes_=0u;key1_reply_payload_encrypted_=false;key1_reply_data_offset_=0u;key1_reply_post_payload_zero_bytes_=0u;key1_post_payload_zero_pending_=false;key1_post_payload_zero_cycles_=0u;}
Status NdsGameCardControl::decode_next_key1_command(
    NdsGameCardKey1Command &decoded) noexcept
{
 if(!installed_)return Status::Unmapped;
 if(!key1_command_mode_||busy_||!chip_id_configured_||
    key1_command_gap_pending_)
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
 if(key1_key2_activation_pending_&&candidate.opcode!=UINT8_C(0x04))
  return Status::PermissionDenied;
 if(candidate.opcode==UINT8_C(0x01)||
    candidate.opcode==UINT8_C(0x06)||
    candidate.opcode==UINT8_C(0x0A)){
  if(!key1_random_l_initialized_||candidate.parameter16!=key1_random_l_)
   return Status::PermissionDenied;
 }else if(candidate.opcode==UINT8_C(0x04)){
  if(key1_random_l_initialized_&&candidate.parameter16!=key1_random_l_)
   return Status::PermissionDenied;
 }

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
  key1_repeat_delay_cycles_remaining_=0u;
 }
 for(uint32_t index=0u;index<CommandBytes;++index)
  last_key1_command_[index]=wire_bytes[index];
 last_key1_command_valid_=true;
 key1_command_gap_pending_=true;
 if(candidate.opcode==UINT8_C(0x04)){
  key1_random_l_=candidate.parameter16;
  key1_random_l_initialized_=true;
  key1_key2_pending_seed24_=candidate.key2_seed24;
  key1_key2_activation_pending_=true;
 }
 decoded=candidate;
 return Status::Ok;
}
Status NdsGameCardControl::describe_key1_reply(
    const NdsGameCardKey1Command &command,
    NdsGameCardKey1ReplyPlan &plan) const noexcept
{
 if(!installed_)return Status::Unmapped;
 if(!key1_command_mode_||!key1_command_gap_pending_||
    !header_protocol_available_||
    command.opcode!=last_key1_command_opcode_||
    static_cast<uint8_t>(command.kind)!=command.opcode||
    command.counter20!=key1_command_counter_)
  return Status::PermissionDenied;

 NdsGameCardKey1ReplyPlan candidate{};
 candidate.transmission_index=last_key1_command_transmissions_;
 candidate.preamble_gap_bytes=header_key1_initial_gap_bytes();
 candidate.preamble_clocked=(rom_control_&RomCtrlGapClockMask)!=0u;
 if(candidate.preamble_clocked&&candidate.preamble_gap_bytes!=0u){
  candidate.leading_high_z_bytes=1u;
  candidate.preamble_key2_zero_bytes=candidate.preamble_gap_bytes-1u;
 }

 switch(command.kind){
 case NdsGameCardKey1CommandKind::ActivateKey2:
  /* 0x40 only installs the KEY2 seed; it has no payload. */
  break;
 case NdsGameCardKey1CommandKind::ReadChipId:
  candidate.aggregate_payload_bytes=4u;
  candidate.aggregate_data_transmissions=UINT8_C(1);
  candidate.payload_bytes=
      newer_protocol_variant()&&candidate.transmission_index==UINT8_C(1)
          ? 0u : UINT32_C(4);
  break;
 case NdsGameCardKey1CommandKind::ReadSecureAreaBlock:
  if(newer_protocol_variant()){
   /* First transmission is a zero-length command. The following eight
    * transmissions return 200h bytes each, separated by 18h KEY2 zeros. */
   candidate.aggregate_payload_bytes=UINT32_C(0x10C0);
   candidate.aggregate_data_transmissions=UINT8_C(8);
   candidate.secure_block_bytes=UINT32_C(0x200);
   candidate.secure_block_count=UINT8_C(8);
   candidate.secure_inter_block_zero_bytes=UINT32_C(0x18);
   candidate.payload_bytes=
       candidate.transmission_index==UINT8_C(1)
           ? 0u : UINT32_C(0x200);
   if(candidate.transmission_index>=UINT8_C(2)&&
      candidate.transmission_index<=UINT8_C(9))
    candidate.post_payload_zero_bytes=UINT32_C(0x18);
  }else{
   candidate.aggregate_payload_bytes=UINT32_C(0x1000);
   candidate.aggregate_data_transmissions=UINT8_C(1);
   candidate.payload_bytes=UINT32_C(0x1000);
   candidate.secure_block_bytes=UINT32_C(0x1000);
   candidate.secure_block_count=UINT8_C(1);
  }
  break;
 case NdsGameCardKey1CommandKind::DisableKey2:
  candidate.payload_length_known=false;
  break;
 case NdsGameCardKey1CommandKind::EnterMainDataMode:
  /* The command has no payload; following commands use KEY2 main-data mode. */
  break;
 default:
  return Status::PermissionDenied;
 }

 plan=candidate;
 return Status::Ok;
}
Status NdsGameCardControl::complete_key1_command_gap(
    uint32_t clock_bytes) noexcept
{
 if(!installed_)return Status::Unmapped;
 if(busy_||!key1_command_mode_||!chip_id_configured_||
    !key1_command_gap_pending_||!header_protocol_available_)
  return Status::PermissionDenied;
 const uint32_t active_gap1=rom_control_&RomCtrlKey1Gap1Mask;
 const uint32_t active_gap2=
     (rom_control_>>RomCtrlKey1Gap2Shift)&RomCtrlKey1Gap2Mask;
 const uint32_t required_clock_bytes=
     (rom_control_&RomCtrlGapClockMask)!=0u
         ? active_gap1+active_gap2 : 0u;
 if(clock_bytes!=required_clock_bytes)return Status::PermissionDenied;

 /* The cart enters KEY2 at the end of the final 0x40 exchange, before
  * the following gap clocks are consumed. */
 if(key1_key2_activation_pending_&&
    last_key1_command_opcode_==UINT8_C(0x04)){
  const uint8_t required_transmissions=
      newer_protocol_variant()?UINT8_C(2):UINT8_C(1);
  if(last_key1_command_transmissions_>=required_transmissions){
   const Status seed_status=key2_.initialize_from_random_seed(
       key1_key2_pending_seed24_,key2_seed_index_);
   if(seed_status!=Status::Ok)return seed_status;
   key1_key2_activation_pending_=false;
   key1_key2_pending_seed24_=0u;
  }
 }

 /* Older carts clock the gap onto the KEY2 stream. The first returned
  * byte is HIGH-Z, but it still consumes one keystream byte; the rest are
  * KEY2-encrypted zero bytes. Newer carts set bit28=0, so clock_bytes=0. */
 if(clock_bytes!=0u){
  if(!key2_.initialized())return Status::PermissionDenied;
  uint8_t zero_bytes[64]{};
  uint32_t remaining=clock_bytes;
  while(remaining!=0u){
   const uint32_t chunk=remaining<sizeof(zero_bytes)
       ? remaining : static_cast<uint32_t>(sizeof(zero_bytes));
   for(uint32_t index=0u;index<chunk;++index)zero_bytes[index]=0u;
   const Status stream_status=key2_.apply_keystream(zero_bytes,chunk);
   if(stream_status!=Status::Ok)return stream_status;
   remaining-=chunk;
  }
 }

 key1_command_gap_pending_=false;
 /* New-cart secure delay starts after the first command's silent gap. */
 if(newer_protocol_variant()&&last_key1_command_transmissions_==1u)
  key1_repeat_delay_cycles_remaining_=header_secure_area_delay_core_cycles();
 return Status::Ok;
}
uint32_t NdsGameCardControl::read_rom_control() const noexcept{uint32_t v=rom_control_;if(data_ready_)v|=RomCtrlDrqMask;if(busy_)v|=RomCtrlStartBusyMask;return v;}
Status NdsGameCardControl::start_key1_command_transfer(NdsCpu cpu) noexcept
{
 if(!header_protocol_available_||!chip_id_configured_||!key1_.initialized())
  return Status::PermissionDenied;
 if((rom_control_&RomCtrlKey2DataMask)!=0u||
    block_bytes((rom_control_>>RomCtrlBlockSizeShift)&kBlockMask)!=0u)
  return Status::PermissionDenied;

 /* Preview first so commands outside the currently wired 0x40/0x10/0x20/0xA0
  * paths are rejected without consuming the KEY1 counter or repeat allowance. */
 std::array<uint8_t,CommandBytes> preview=command_;
 const Status preview_status=key1_.decrypt_command(preview.data(),CommandBytes);
 if(preview_status!=Status::Ok)return preview_status;
 const uint8_t preview_opcode=static_cast<uint8_t>(preview[0]>>4u);
 if(preview_opcode!=UINT8_C(0x04)&&preview_opcode!=UINT8_C(0x01)&&
    preview_opcode!=UINT8_C(0x02)&&preview_opcode!=UINT8_C(0x0A))
  return Status::PermissionDenied;
 uint16_t preview_parameter16=static_cast<uint16_t>(
     (static_cast<uint16_t>(preview[0]&UINT8_C(0x0F))<<12u)|
     (static_cast<uint16_t>(preview[1])<<4u)|
     (static_cast<uint16_t>(preview[2])>>4u));
 uint32_t secure_area_base=0u;
 if(preview_opcode==UINT8_C(0x02)){
  if(preview_parameter16<UINT16_C(4)||preview_parameter16>UINT16_C(7))
   return Status::PermissionDenied;
  secure_area_base=static_cast<uint32_t>(preview_parameter16)<<12u;
  if(static_cast<uint64_t>(secure_area_base)+UINT32_C(0x1000)>image_size_)
   return Status::AddressOverflow;
 }

 const bool gap_clocked=(rom_control_&RomCtrlGapClockMask)!=0u;
 const uint32_t active_gap_bytes=
     (rom_control_&RomCtrlKey1Gap1Mask)+
     ((rom_control_>>RomCtrlKey1Gap2Shift)&RomCtrlKey1Gap2Mask);
 if(gap_clocked&&active_gap_bytes!=header_key1_initial_gap_bytes())
  return Status::PermissionDenied;

 NdsGameCardKey1Command decoded{};
 const Status decode_status=decode_next_key1_command(decoded);
 if(decode_status!=Status::Ok)return decode_status;
 NdsGameCardKey1ReplyPlan plan{};
 const Status plan_status=describe_key1_reply(decoded,plan);
 if(plan_status!=Status::Ok||!plan.payload_length_known)
  return Status::PermissionDenied;
 if(decoded.kind==NdsGameCardKey1CommandKind::ActivateKey2){
  if(plan.payload_bytes!=0u)return Status::PermissionDenied;
 }else if(decoded.kind==NdsGameCardKey1CommandKind::ReadChipId){
  if(!key2_.initialized()||
     (plan.payload_bytes!=0u&&plan.payload_bytes!=kWordBytes)||
     !plan.payload_key2_encrypted)
   return Status::PermissionDenied;
 }else if(decoded.kind==NdsGameCardKey1CommandKind::ReadSecureAreaBlock){
  if(!key2_.initialized()||!plan.payload_key2_encrypted||
     plan.secure_block_count!=(newer_protocol_variant()?UINT8_C(8):UINT8_C(1))||
     plan.secure_block_bytes!=(newer_protocol_variant()?UINT32_C(0x200):UINT32_C(0x1000))||
     (newer_protocol_variant()&&
       (plan.aggregate_payload_bytes!=UINT32_C(0x10C0)||
        plan.aggregate_data_transmissions!=UINT8_C(8)||
        (plan.payload_bytes!=0u&&plan.payload_bytes!=UINT32_C(0x200))))||
     (!newer_protocol_variant()&&
       (plan.payload_bytes!=UINT32_C(0x1000)||
        plan.aggregate_payload_bytes!=UINT32_C(0x1000))))
   return Status::PermissionDenied;
 }else if(decoded.kind==NdsGameCardKey1CommandKind::EnterMainDataMode){
  if(!key2_.initialized()||plan.payload_bytes!=0u||
     !plan.payload_length_known)
   return Status::PermissionDenied;
 }else{
  return Status::PermissionDenied;
 }

 cycles_per_byte_=(rom_control_&RomCtrlClockSlowMask)!=0u?8u:5u;
 key1_gap_cycles_=static_cast<uint64_t>(plan.preamble_gap_bytes)*
     cycles_per_byte_;
 key1_gap_clock_bytes_=plan.preamble_clocked
     ? plan.preamble_gap_bytes : 0u;
 key1_reply_payload_bytes_=plan.payload_bytes;
 key1_reply_payload_encrypted_=plan.payload_key2_encrypted;
 if(decoded.kind==NdsGameCardKey1CommandKind::EnterMainDataMode)
  key1_main_data_mode_pending_=true;
 key1_reply_data_offset_=0u;
 key1_reply_post_payload_zero_bytes_=plan.post_payload_zero_bytes;
 if(decoded.kind==NdsGameCardKey1CommandKind::ReadSecureAreaBlock){
  key1_reply_data_offset_=secure_area_base;
  if(newer_protocol_variant()&&plan.transmission_index>=UINT8_C(2))
   key1_reply_data_offset_+=
       (static_cast<uint32_t>(plan.transmission_index)-UINT32_C(2))*
       plan.secure_block_bytes;
 }
 key1_command_bytes_pending_=true;
 key1_gap_transfer_active_=true;
 remaining_words_=0u;
 current_offset_=0u;
 current_window_base_=0u;
 dummy_data_=false;
 chip_id_data_=false;
 wrap_read_window_=false;
 key2_data_active_=false;
 cycles_until_data_=static_cast<uint64_t>(CommandBytes)*cycles_per_byte_;
 owner_cpu_=cpu;
 busy_=true;
 data_ready_=false;
 return Status::Ok;
}
Status NdsGameCardControl::start_key2_command_transfer(NdsCpu cpu) noexcept
{
 if(!key2_command_mode_||!key2_.initialized()||!chip_id_configured_||
    (rom_control_&RomCtrlKey2CommandMask)==0u||
    (rom_control_&RomCtrlKey2DataMask)==0u||
    (rom_control_&RomCtrlWriteMask)!=0u)
  return Status::PermissionDenied;

 const uint32_t block_code=(rom_control_>>RomCtrlBlockSizeShift)&kBlockMask;
 const uint32_t size=block_bytes(block_code);
 if(size==0u||(size%kWordBytes)!=0u)return Status::PermissionDenied;

 /* Validate on a copy first. Invalid commands must not consume the live
  * KEY2 stream, otherwise every following command would be desynchronized. */
 std::array<uint8_t,CommandBytes> decoded=command_;
 NdsGameCardKey2 preview_stream=key2_;
 const Status preview_status=preview_stream.apply_keystream(
     decoded.data(),CommandBytes);
 if(preview_status!=Status::Ok)return preview_status;

 uint32_t offset=0u;
 bool wrap_window=false;
 bool chip_id_reply=false;
 if(decoded[0]==UINT8_C(0xB7)){
  if(size<UINT32_C(0x200)||size>kRetailWindowBytes||
     decoded[5]!=0u||decoded[6]!=0u||decoded[7]!=0u)
   return Status::PermissionDenied;
  const uint32_t requested_offset=
      (static_cast<uint32_t>(decoded[1])<<24u)|
      (static_cast<uint32_t>(decoded[2])<<16u)|
      (static_cast<uint32_t>(decoded[3])<<8u)|
      static_cast<uint32_t>(decoded[4]);
  /* KEY2 main-data reads cannot address the protected Secure Area. */
  if(requested_offset<kRetailRedirectBase)
   return Status::PermissionDenied;
  offset=requested_offset;
  wrap_window=true;
 }else if(decoded[0]==UINT8_C(0xB8)){
  if(size!=kWordBytes)return Status::PermissionDenied;
  for(uint32_t index=1u;index<CommandBytes;++index)
   if(decoded[index]!=0u)return Status::PermissionDenied;
  chip_id_reply=true;
 }else{
  return Status::PermissionDenied;
 }

 const uint32_t window_base=wrap_window
     ? offset&~(kRetailWindowBytes-1u) : 0u;
 if(wrap_window){
  for(uint32_t byte=0u;byte<size;++byte){
   const uint64_t address=static_cast<uint64_t>(window_base)|
       ((offset-window_base+byte)&(kRetailWindowBytes-1u));
   if(address>=image_size_)return Status::AddressOverflow;
  }
 }

 const Status command_status=
     key2_.apply_keystream(command_.data(),CommandBytes);
 if(command_status!=Status::Ok)return command_status;

 const uint32_t gap1=rom_control_&RomCtrlKey1Gap1Mask;
 const uint32_t gap2=
     (rom_control_>>RomCtrlKey1Gap2Shift)&RomCtrlKey1Gap2Mask;
 cycles_per_byte_=(rom_control_&RomCtrlClockSlowMask)!=0u?8u:5u;
 current_offset_=offset;
 current_window_base_=window_base;
 remaining_words_=size/kWordBytes;
 chip_id_data_=chip_id_reply;
 dummy_data_=false;
 wrap_read_window_=wrap_window;
 key2_data_active_=true;
 key2_command_transfer_active_=true;
 key2_transfer_data_bytes_=0u;
 key2_transfer_gap2_bytes_=gap2;
 cycles_until_data_=static_cast<uint64_t>(CommandBytes+gap1+gap2+kWordBytes)*
     cycles_per_byte_;
 owner_cpu_=cpu;
 busy_=true;
 data_ready_=false;
 return Status::Ok;
}
Status NdsGameCardControl::start_transfer(NdsCpu cpu) noexcept{
 if(!installed_)return Status::Unmapped;if(!valid_cpu(cpu))return Status::InvalidCpu;
 if(busy_||!reset_released_||!image_)return Status::PermissionDenied;
 if((aux_spi_control_&AuxSpiSlotEnableMask)==0u ||
    (aux_spi_control_&AuxSpiSlotModeMask)!=0u)return Status::PermissionDenied;
 if((rom_control_&RomCtrlWriteMask)!=0u)return Status::PermissionDenied;
 if(key1_command_mode_){
  if((rom_control_&RomCtrlKey2CommandMask)!=0u)return Status::PermissionDenied;
  return start_key1_command_transfer(cpu);
 }
 if(key2_command_mode_)return start_key2_command_transfer(cpu);
 if((rom_control_&RomCtrlKey2CommandMask)!=0u)return Status::PermissionDenied;
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
 if(key2_command_transfer_active_)key2_transfer_data_bytes_+=kWordBytes;
 if(remaining_words_==0u){
  if(key1_reply_post_payload_zero_bytes_!=0u&&key2_data_active_){
   key1_post_payload_zero_pending_=true;
   key1_post_payload_zero_cycles_=
       static_cast<uint64_t>(key1_reply_post_payload_zero_bytes_)*cycles_per_byte_;
   cycles_until_data_=key1_post_payload_zero_cycles_;
  }else{
   finish_transfer();
  }
 }else{
  uint64_t delay_bytes=kWordBytes;
  if(key2_command_transfer_active_&&key2_transfer_gap2_bytes_!=0u&&
     (key2_transfer_data_bytes_%UINT32_C(0x200))==0u)
   delay_bytes+=key2_transfer_gap2_bytes_;
  cycles_until_data_=delay_bytes*cycles_per_byte_;
 }
 return{Status::Ok,value};
}
void NdsGameCardControl::finish_transfer() noexcept{busy_=false;data_ready_=false;dummy_data_=false;chip_id_data_=false;wrap_read_window_=false;key2_data_active_=false;current_window_base_=0u;cycles_until_data_=0u;key1_gap_transfer_active_=false;key1_command_bytes_pending_=false;key1_gap_clock_bytes_=0u;key1_gap_cycles_=0u;key1_reply_payload_bytes_=0u;key1_reply_payload_encrypted_=false;key1_reply_data_offset_=0u;key1_reply_post_payload_zero_bytes_=0u;key1_post_payload_zero_pending_=false;key1_post_payload_zero_cycles_=0u;key2_command_transfer_active_=false;key2_transfer_data_bytes_=0u;key2_transfer_gap2_bytes_=0u;if(key1_activation_pending_){key1_command_mode_=true;key1_activation_pending_=false;}if(key1_main_data_mode_pending_&&last_key1_command_opcode_==UINT8_C(0x0A)){const uint8_t required_transmissions=newer_protocol_variant()?UINT8_C(2):UINT8_C(1);if(last_key1_command_transmissions_>=required_transmissions){key1_main_data_mode_pending_=false;key1_command_mode_=false;key2_command_mode_=true;}}if(aux_spi_control_&AuxSpiIrqEnableMask)(void)interrupts_.request_irq(owner_cpu_,GameCardTransferIrqMask);}
void NdsGameCardControl::advance_bus_cycles(uint64_t cycles) noexcept{
 if(!installed_||cycles==0u)return;
 if(key1_repeat_delay_cycles_remaining_!=0u){
  if(cycles>=key1_repeat_delay_cycles_remaining_)
   key1_repeat_delay_cycles_remaining_=0u;
  else
   key1_repeat_delay_cycles_remaining_-=static_cast<uint32_t>(cycles);
 }
 if(key1_post_payload_zero_pending_&&busy_){
  if(cycles<cycles_until_data_){cycles_until_data_-=cycles;return;}
  cycles-=cycles_until_data_;cycles_until_data_=0u;
  uint8_t zero_bytes[UINT32_C(0x18)]{};
  if(key1_reply_post_payload_zero_bytes_==0u||
     key1_reply_post_payload_zero_bytes_>sizeof(zero_bytes)||
     key2_.apply_keystream(zero_bytes,
         key1_reply_post_payload_zero_bytes_)!=Status::Ok){
   finish_transfer();
   return;
  }
  finish_transfer();
  return;
 }
 if(key1_gap_transfer_active_&&busy_){
  while(busy_){
   if(cycles<cycles_until_data_){cycles_until_data_-=cycles;return;}
   cycles-=cycles_until_data_;cycles_until_data_=0u;
   if(key1_command_bytes_pending_){
    key1_command_bytes_pending_=false;
    cycles_until_data_=key1_gap_cycles_;
    key1_gap_cycles_=0u;
    continue;
   }
   /* complete_key1_command_gap() runs at the gap boundary, after BUSY drops. */
   busy_=false;
   const Status gap_status=complete_key1_command_gap(key1_gap_clock_bytes_);
   if(gap_status!=Status::Ok){
    key1_gap_transfer_active_=false;
    key1_command_bytes_pending_=false;
    key1_gap_clock_bytes_=0u;
    key1_gap_cycles_=0u;
    return;
   }
   if(key1_reply_payload_bytes_!=0u){
    const uint32_t payload_bytes=key1_reply_payload_bytes_;
    const bool payload_encrypted=key1_reply_payload_encrypted_;
    const bool chip_id_payload=last_key1_command_opcode_==UINT8_C(0x01);
    const bool secure_area_payload=last_key1_command_opcode_==UINT8_C(0x02);
    if((payload_bytes%kWordBytes)!=0u||
       (!chip_id_payload&&!secure_area_payload)||
       (chip_id_payload&&payload_bytes!=kWordBytes)||
       (secure_area_payload&&payload_bytes!=UINT32_C(0x200)&&
        payload_bytes!=UINT32_C(0x1000))||
       (secure_area_payload&&
        static_cast<uint64_t>(key1_reply_data_offset_)+payload_bytes>image_size_)){
     key1_gap_transfer_active_=false;
     key1_command_bytes_pending_=false;
     key1_gap_clock_bytes_=0u;
     key1_gap_cycles_=0u;
     key1_reply_payload_bytes_=0u;
     key1_reply_payload_encrypted_=false;
     finish_transfer();
     return;
    }
    busy_=true;
    remaining_words_=payload_bytes/kWordBytes;
    chip_id_data_=chip_id_payload;
    if(secure_area_payload)current_offset_=key1_reply_data_offset_;
    dummy_data_=false;
    wrap_read_window_=false;
    key2_data_active_=payload_encrypted;
    key1_gap_transfer_active_=false;
    key1_command_bytes_pending_=false;
    key1_gap_clock_bytes_=0u;
    key1_gap_cycles_=0u;
    key1_reply_payload_bytes_=0u;
    key1_reply_payload_encrypted_=false;
    key1_reply_data_offset_=0u;
    cycles_until_data_=static_cast<uint64_t>(kWordBytes)*cycles_per_byte_;
    if(key1_repeat_delay_cycles_remaining_!=0u&&cycles!=0u){
     if(cycles>=key1_repeat_delay_cycles_remaining_)
      key1_repeat_delay_cycles_remaining_=0u;
     else
      key1_repeat_delay_cycles_remaining_-=static_cast<uint32_t>(cycles);
    }
    break;
   }
   finish_transfer();
   if(key1_repeat_delay_cycles_remaining_!=0u&&cycles!=0u){
    if(cycles>=key1_repeat_delay_cycles_remaining_)
     key1_repeat_delay_cycles_remaining_=0u;
    else
     key1_repeat_delay_cycles_remaining_-=static_cast<uint32_t>(cycles);
   }
   return;
  }
  return;
 }
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
