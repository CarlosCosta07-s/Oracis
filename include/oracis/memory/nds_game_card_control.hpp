/* Original NDS Game Card controller: raw, unencrypted B7 reads only. */
#ifndef ORACIS_MEMORY_NDS_GAME_CARD_CONTROL_HPP
#define ORACIS_MEMORY_NDS_GAME_CARD_CONTROL_HPP
#include <array>
#include <cstdint>
#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"
namespace oracis::memory {
class NdsDmaControl;
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
 static constexpr uint8_t RomReadCommand=UINT8_C(0xB7);
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
 void unmount_image() noexcept;
 ReadResult read(NdsCpu cpu,uint32_t address,uint32_t width) noexcept;
 Status write(NdsCpu cpu,uint32_t address,uint32_t width,uint32_t value) noexcept;
 void advance_bus_cycles(uint64_t cycles) noexcept;
 void attach_dma_control(NdsDmaControl &dma) noexcept { dma_control_ = &dma; }
 ReadResult read_data(NdsCpu cpu) noexcept;
 void reset_transfer() noexcept;
 bool installed() const noexcept{return installed_;}
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
 void finish_transfer() noexcept;
 NdsInterruptControl &interrupts_;
 NdsDmaControl *dma_control_=nullptr;
 const uint8_t *image_=nullptr;
 uint32_t image_size_=0u;
 std::array<uint8_t,CommandBytes> command_{};
 std::array<uint8_t,SeedBytes> seed_registers_{};
 uint16_t aux_spi_control_=0u;
 uint32_t rom_control_=0u;
 uint32_t current_offset_=0u;
 uint32_t current_window_base_=0u;
 uint32_t remaining_words_=0u;
 uint64_t cycles_until_data_=0u;
 uint32_t cycles_per_byte_=5u;
 NdsCpu owner_cpu_=NDS_ARM9;
 bool reset_released_=false,busy_=false,data_ready_=false,dummy_data_=false,wrap_read_window_=false,installed_=false;
};
}
#endif
