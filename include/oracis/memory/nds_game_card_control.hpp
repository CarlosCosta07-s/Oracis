/* Original NDS Game Card controller with a seed-driven KEY2 data path. */
#ifndef ORACIS_MEMORY_NDS_GAME_CARD_CONTROL_HPP
#define ORACIS_MEMORY_NDS_GAME_CARD_CONTROL_HPP
#include <array>
#include <cstdint>
#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/nds_game_card_key1.hpp"
#include "oracis/memory/nds_game_card_key2.hpp"
namespace oracis::memory {
class NdsDmaControl;

struct NdsGameCardKey1ReplyPlan {
 uint8_t transmission_index=0u;
 uint32_t preamble_gap_bytes=0u;
 bool preamble_clocked=false;
 uint32_t leading_high_z_bytes=0u;
 uint32_t preamble_key2_zero_bytes=0u;
 /* Payload for this command transmission versus the full command sequence. */
 uint32_t payload_bytes=0u;
 uint32_t aggregate_payload_bytes=0u;
 uint8_t aggregate_data_transmissions=0u;
 bool payload_key2_encrypted=true;
 uint32_t secure_block_bytes=0u;
 uint8_t secure_block_count=0u;
 uint32_t secure_inter_block_zero_bytes=0u;
 uint32_t post_payload_zero_bytes=0u;
 bool payload_length_known=true;
};

class NdsGameCardControl final {
public:
 static constexpr uint32_t AuxSpiControlAddress=UINT32_C(0x040001A0);
 static constexpr uint32_t RomControlAddress=UINT32_C(0x040001A4);
 static constexpr uint32_t CommandAddress=UINT32_C(0x040001A8);
 static constexpr uint32_t CommandBytes=8u;
 static constexpr uint32_t SeedAddress=UINT32_C(0x040001B0);
 static constexpr uint32_t SeedBytes=12u;
 static constexpr uint32_t DataAddress=UINT32_C(0x04100010);
 static constexpr uint32_t GameCardTransferIrqMask=UINT32_C(1)<<19u;
 static constexpr uint8_t DummyCommand=UINT8_C(0x9F);
 static constexpr uint8_t HeaderReadCommand=UINT8_C(0x00);
 static constexpr uint8_t ChipIdReadCommand=UINT8_C(0x90);
 static constexpr uint8_t RomReadCommand=UINT8_C(0xB7);
 static constexpr uint8_t ActivateKey1Command=UINT8_C(0x3C);
 static constexpr uint32_t HeaderKey2SeedIndexOffset=UINT32_C(0x13);
 static constexpr uint32_t HeaderNormalRomControlOffset=UINT32_C(0x60);
 static constexpr uint32_t HeaderKey1RomControlOffset=UINT32_C(0x64);
 static constexpr uint32_t HeaderSecureAreaDelayOffset=UINT32_C(0x6E);
 static constexpr uint32_t RomCtrlKey1Gap1Mask=UINT32_C(0x1FFF);
 static constexpr uint32_t RomCtrlKey1Gap2Shift=16u;
 static constexpr uint32_t RomCtrlKey1Gap2Mask=UINT32_C(0x3F);
 static constexpr uint32_t RomCtrlKey2DataMask=UINT32_C(1)<<13u;
 static constexpr uint32_t RomCtrlKey2SeedApplyMask=UINT32_C(1)<<15u;
 static constexpr uint32_t RomCtrlKey2CommandMask=UINT32_C(1)<<22u;
 static constexpr uint32_t RomCtrlDrqMask=UINT32_C(1)<<23u;
 static constexpr uint32_t RomCtrlBlockSizeShift=24u;
 static constexpr uint32_t RomCtrlClockSlowMask=UINT32_C(1)<<27u;
 static constexpr uint32_t RomCtrlGapClockMask=UINT32_C(1)<<28u;
 static constexpr uint32_t RomCtrlResetReleaseMask=UINT32_C(1)<<29u;
 static constexpr uint32_t RomCtrlWriteMask=UINT32_C(1)<<30u;
 static constexpr uint32_t RomCtrlStartBusyMask=UINT32_C(1)<<31u;
 static constexpr uint32_t AuxSpiSlotModeMask=UINT32_C(1)<<13u;
 static constexpr uint32_t AuxSpiIrqEnableMask=UINT32_C(1)<<14u;
 static constexpr uint32_t AuxSpiSlotEnableMask=UINT32_C(1)<<15u;
 explicit NdsGameCardControl(NdsInterruptControl &irq) noexcept:interrupts_(irq){}
 NdsGameCardControl(const NdsGameCardControl&)=delete;
 NdsGameCardControl& operator=(const NdsGameCardControl&)=delete;
 Status install() noexcept;
 /* Image bytes remain caller-owned and must outlive the mount. */
 Status mount_image(const uint8_t *bytes,uint32_t size) noexcept;
 /* Copy the 0x1048-byte KEY1 table extracted from the user-owned ARM7 BIOS. */
 Status load_key1_key_table(const uint8_t *bytes,uint32_t size) noexcept;
 /* Chip ID is cartridge metadata not present in every .nds dump; caller must supply it. */
 Status set_chip_id(uint32_t chip_id) noexcept;
 void unmount_image() noexcept;
 ReadResult read(NdsCpu cpu,uint32_t address,uint32_t width) noexcept;
 Status write(NdsCpu cpu,uint32_t address,uint32_t width,uint32_t value) noexcept;
 void advance_bus_cycles(uint64_t cycles) noexcept;
 void attach_dma_control(NdsDmaControl &dma) noexcept { dma_control_ = &dma; }
 ReadResult read_data(NdsCpu cpu) noexcept;
 void reset_transfer() noexcept;
 /* Validate and consume a KEY1 command without starting its data/gap transfer.
  * advance_bus_cycles() drives the header secure-delay countdown. */
 Status decode_next_key1_command(NdsGameCardKey1Command &decoded) noexcept;
 Status describe_key1_reply(const NdsGameCardKey1Command &command,
     NdsGameCardKey1ReplyPlan &plan) const noexcept;
 /* Complete the gap after a decoded KEY1 command. clock_bytes must match the
  * live ROMCTRL gap settings and the clocks actually emitted by the bus. */
 Status complete_key1_command_gap(uint32_t clock_bytes) noexcept;
 bool installed() const noexcept{return installed_;}
 bool key2_initialized() const noexcept{return key2_.initialized();}
 bool key1_initialized() const noexcept{return key1_.initialized();}
 bool key1_command_mode() const noexcept{return key1_command_mode_;}
 uint32_t key1_id_code() const noexcept{return key1_.id_code();}
 uint16_t key1_random_i() const noexcept{return key1_random_i_;}
 uint16_t key1_random_j() const noexcept{return key1_random_j_;}
 uint16_t key1_random_l() const noexcept{return key1_random_l_;}
 bool key1_random_l_initialized() const noexcept{return key1_random_l_initialized_;}
 uint32_t key1_command_counter() const noexcept{return key1_command_counter_;}
 uint32_t key1_repeat_delay_cycles_remaining() const noexcept{return key1_repeat_delay_cycles_remaining_;}
 bool key1_key2_activation_pending() const noexcept{return key1_key2_activation_pending_;}
 bool chip_id_configured() const noexcept{return chip_id_configured_;}
 uint32_t chip_id() const noexcept{return chip_id_;}
 bool newer_protocol_variant() const noexcept{return chip_id_configured_&&(chip_id_&UINT32_C(0x80000000))!=0u;}
 bool header_protocol_available() const noexcept{return header_protocol_available_;}
 uint32_t header_normal_rom_control() const noexcept{return header_normal_rom_control_;}
 uint32_t header_key1_rom_control() const noexcept{return header_key1_rom_control_;}
 uint32_t header_key1_gap1_length() const noexcept{return header_key1_rom_control_&RomCtrlKey1Gap1Mask;}
 uint32_t header_key1_gap2_length() const noexcept{return (header_key1_rom_control_>>RomCtrlKey1Gap2Shift)&RomCtrlKey1Gap2Mask;}
 bool header_key1_gap_clocks_enabled() const noexcept{return (header_key1_rom_control_&RomCtrlGapClockMask)!=0u;}
 bool header_key1_slow_clock() const noexcept{return (header_key1_rom_control_&RomCtrlClockSlowMask)!=0u;}
 uint32_t header_key1_initial_gap_bytes() const noexcept{return header_key1_gap1_length()+header_key1_gap2_length();}
 uint16_t header_secure_area_delay() const noexcept{return header_secure_area_delay_;}
 uint32_t header_secure_area_delay_core_cycles() const noexcept{return static_cast<uint32_t>(header_secure_area_delay_)*UINT32_C(256);}
 uint8_t header_key2_seed_index() const noexcept{return key2_seed_index_;}
 uint64_t key2_state1() const noexcept{return key2_.state1();}
 uint64_t key2_state2() const noexcept{return key2_.state2();}
 uint64_t key2_bytes_processed() const noexcept{return key2_.bytes_processed();}
 bool mounted() const noexcept{return image_!=nullptr;}
 bool busy() const noexcept{return busy_;}
 bool data_ready() const noexcept{return data_ready_;}
 uint32_t remaining_words() const noexcept{return remaining_words_;}
 NdsCpu owner_cpu() const noexcept{return owner_cpu_;}
 NdsInterruptControl& interrupt_control() const noexcept{return interrupts_;}
private:
 static bool valid_cpu(NdsCpu cpu) noexcept;
 static bool command_address(uint32_t address) noexcept;
 static bool seed_address(uint32_t address) noexcept;
 uint32_t read_rom_control() const noexcept;
 Status write_rom_control(NdsCpu cpu,uint32_t value) noexcept;
 Status start_transfer(NdsCpu cpu) noexcept;
 Status start_key1_command_transfer(NdsCpu cpu) noexcept;
 Status initialize_key1_from_image() noexcept;
 void finish_transfer() noexcept;
 NdsInterruptControl &interrupts_;
 NdsDmaControl *dma_control_=nullptr;
 const uint8_t *image_=nullptr;
 uint32_t image_size_=0u;
 std::array<uint8_t,CommandBytes> command_{};
 std::array<std::array<uint8_t,SeedBytes>,2> seed_registers_{};
 NdsGameCardKey2 key2_{};
 NdsGameCardKey1 key1_{};
 uint16_t aux_spi_control_=0u;
 uint32_t rom_control_=0u;
 uint32_t current_offset_=0u;
 uint32_t current_window_base_=0u;
 uint32_t remaining_words_=0u;
 uint64_t cycles_until_data_=0u;
 uint32_t cycles_per_byte_=5u;
 NdsCpu owner_cpu_=NDS_ARM9;
 uint32_t chip_id_=0u;
 uint32_t header_normal_rom_control_=0u,header_key1_rom_control_=0u;
 uint16_t header_secure_area_delay_=0u;
 uint8_t key2_seed_index_=0u;
 bool header_protocol_available_=false;
 bool chip_id_configured_=false;
 bool chip_id_data_=false;
 bool reset_released_=false,busy_=false,data_ready_=false,dummy_data_=false,wrap_read_window_=false,installed_=false,key2_data_active_=false;
 bool key1_command_mode_=false,key1_activation_pending_=false;
 uint16_t key1_random_i_=0u,key1_random_j_=0u,key1_random_l_=0u;
 bool key1_random_l_initialized_=false;
 uint32_t key1_command_counter_=0u;
 std::array<uint8_t,CommandBytes> last_key1_command_{};
 bool last_key1_command_valid_=false;
 uint8_t last_key1_command_transmissions_=0u;
 uint8_t last_key1_command_opcode_=0u;
 uint32_t key1_repeat_delay_cycles_remaining_=0u;
 uint32_t key1_key2_pending_seed24_=0u;
 bool key1_key2_activation_pending_=false;
 bool key1_command_gap_pending_=false;
 bool key1_gap_transfer_active_=false;
 bool key1_command_bytes_pending_=false;
 uint32_t key1_gap_clock_bytes_=0u;
 uint64_t key1_gap_cycles_=0u;
 uint32_t key1_reply_payload_bytes_=0u;
 bool key1_reply_payload_encrypted_=false;
};
}
#endif
