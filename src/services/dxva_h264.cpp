#include "x64arm/dxva_h264.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <string>

// 欄位與 zigzag 排序依公開 DXVA ABI 核對：
// https://github.com/FFmpeg/FFmpeg/blob/946fcce07b/libavcodec/dxva2_h264.c
// 合成參數集依 H.264 syntax 與 level 限制，不沿用宿主 C 結構記憶體配置：
// https://github.com/FFmpeg/FFmpeg/blob/946fcce07b/libavcodec/cbs_h264_syntax_template.c
// https://github.com/FFmpeg/FFmpeg/blob/946fcce07b/libavcodec/h264_levels.c
namespace x64arm::video {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw DxvaH264Error(message);
}
std::uint32_t le(std::span<const std::uint8_t> bytes, std::size_t offset, unsigned count) {
    require(offset <= bytes.size() && count <= bytes.size() - offset, "DXVA H.264 結構截斷");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= std::uint32_t{bytes[offset + i]} << (8U * i);
    return value;
}
int signed_byte(std::uint8_t value) { return std::bit_cast<std::int8_t>(value); }
std::int32_t signed_dword(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return std::bit_cast<std::int32_t>(le(bytes, offset, 4));
}

class Bits {
public:
    explicit Bits(std::span<const std::uint8_t> source) : bytes_(source) {}
    std::uint32_t read(unsigned count) {
        require(count <= 32 && position_ <= bytes_.size() * 8 && count <= bytes_.size() * 8 - position_, "H.264 slice header 截斷");
        std::uint32_t result = 0;
        for (unsigned i = 0; i < count; ++i, ++position_)
            result = (result << 1) | ((bytes_[position_ / 8] >> (7U - static_cast<unsigned>(position_ % 8))) & 1U);
        return result;
    }
    std::uint32_t ue() {
        unsigned zeroes = 0;
        while (read(1) == 0) { ++zeroes; require(zeroes <= 31, "H.264 Exp-Golomb 超出 32 bits"); }
        return ((std::uint32_t{1} << zeroes) - 1) + read(zeroes);
    }
    std::int64_t se() {
        const auto value = std::uint64_t{ue()};
        return (value & 1) ? static_cast<std::int64_t>((value + 1) / 2) : -static_cast<std::int64_t>(value / 2);
    }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_{};
};

class Writer {
public:
    void put(unsigned count, std::uint32_t value) {
        for (unsigned i = count; i > 0; --i) {
            if (used_ == 0) bytes_.push_back(0);
            bytes_.back() |= static_cast<std::uint8_t>(((value >> (i - 1)) & 1U) << (7U - used_));
            used_ = (used_ + 1) % 8;
        }
    }
    void ue(std::uint32_t value) {
        require(value != UINT32_MAX, "H.264 Exp-Golomb 寫入溢位");
        const auto code = value + 1;
        const auto bits = static_cast<unsigned>(std::bit_width(code));
        put(bits - 1, 0); put(bits, code);
    }
    void se(int value) { ue(static_cast<std::uint32_t>(value <= 0 ? -2 * value : 2 * value - 1)); }
    std::vector<std::uint8_t> nal(std::uint8_t header) {
        put(1, 1);
        if (used_) put(8 - used_, 0);
        std::vector<std::uint8_t> result{header};
        unsigned zeroes = 0;
        for (const auto byte : bytes_) {
            if (zeroes == 2 && byte <= 3) { result.push_back(3); zeroes = 0; }
            result.push_back(byte);
            zeroes = byte == 0 ? zeroes + 1 : 0;
        }
        return result;
    }
private:
    std::vector<std::uint8_t> bytes_;
    unsigned used_{};
};

std::vector<std::uint8_t> rbsp(std::span<const std::uint8_t> nal) {
    require(nal.size() >= 2 && !(nal[0] & 0x80), "H.264 NAL header 無效");
    std::vector<std::uint8_t> result;
    result.reserve(nal.size() - 1);
    unsigned zeroes = 0;
    for (std::size_t i = 1; i < nal.size(); ++i) {
        const auto byte = nal[i];
        if (zeroes == 2) {
            if (byte == 3) {
                // H.264 7.4.1：CABAC zero word 可使 NAL 以 00 00 03 結尾。
                // 最後的 03 仍是 EPB；sample 保留原 bytes，解析時僅移除它。
                require(i + 1 == nal.size() || nal[i + 1] <= 3, "H.264 emulation-prevention byte 無效");
                zeroes = 0;
                continue;
            }
            require(byte > 2, "H.264 NAL 含未跳脫 start-code 序列");
        }
        result.push_back(byte);
        zeroes = byte == 0 ? zeroes + 1 : 0;
    }
    return result;
}

struct Parameters {
    std::uint32_t width_mbs{}, height_mbs{}, refs{}, flags{}, level{}, frame_bits{}, poc_bits{}, poc_type{};
    int qp{}, qs{}, chroma{}, second_chroma{};
    std::uint32_t default_l0{}, default_l1{};
    bool entropy{}, pic_order_present{}, direct8{}, deblock{}, redundant{}, transform8{};
};

Parameters parameters(std::span<const std::uint8_t> p) {
    require(p.size() == dxva_h264_picture_bytes, "DXVA_PicParams_H264 必須為 1040 bytes");
    Parameters result;
    result.width_mbs = le(p, 0, 2) + 1; result.height_mbs = le(p, 2, 2) + 1;
    result.refs = p[5]; result.flags = le(p, 6, 2);
    require(!(result.flags & 0x000f) && (result.flags & 0x1000), "DXVA H.264 尚未支援 field/MBAFF/SP/獨立色彩平面");
    require(((result.flags >> 4) & 3) == 1 && p[8] == 0 && p[9] == 0, "DXVA H.264 僅支援 8-bit 4:2:0");
    require(result.flags & 0x800, "DXVA H.264 尚未支援非連續 macroblock/FMO/ASO");
    require(result.refs <= 16 && p[223] == 0 && p[224] == 0 && le(p, 228, 2) == 0, "DXVA H.264 參考數量無效或使用 FMO");
    require(le(p, 212, 2) == 0, "DXVA H.264 尚未支援 non-existing reference frames");
    require(p[171] == 1, "DXVA H.264 尚未支援分段 picture continuation");
    require(p[175] == 0 && p[227] == 0, "DXVA H.264 reserved 欄位非零");
    require(le(p, 10, 2) == 3, "DXVA H.264 Qmatrix 排序必須為標準 zigzag（Reserved16Bits=3）");
    require(p[216] <= 12 && p[218] <= 12, "DXVA H.264 frame_num/POC 位寬無效");
    result.frame_bits = std::uint32_t{p[216]} + 4; result.poc_bits = std::uint32_t{p[218]} + 4;
    result.poc_type = p[217];
    require(result.poc_type == 0 || result.poc_type == 2, "DXVA H.264 POC type1 缺少 SPS cycle offsets，尚未支援");
    require(p[219] == 0, "DXVA H.264 POC type0/2 不應有 delta_pic_order_always_zero");
    for (const auto offset : {220U, 221U, 222U, 225U, 226U}) require(p[offset] <= 1, "DXVA H.264 boolean 欄位無效");
    require(p[226] == 0, "DXVA H.264 High profile 不支援 redundant pictures");
    result.qs = signed_byte(p[168]); result.chroma = signed_byte(p[169]); result.second_chroma = signed_byte(p[170]);
    result.qp = signed_byte(p[172]); result.default_l0 = p[173]; result.default_l1 = p[174];
    require(result.qp >= -26 && result.qp <= 25 && result.qs >= -26 && result.qs <= 25, "DXVA H.264 QP/QS 超出範圍");
    require(result.chroma >= -12 && result.chroma <= 12 && result.second_chroma >= -12 && result.second_chroma <= 12, "DXVA H.264 chroma QP 超出範圍");
    require(result.default_l0 <= 31 && result.default_l1 <= 31 && ((result.flags >> 9) & 3) <= 2, "DXVA H.264 reference defaults/weighted_bipred 無效");
    result.entropy = p[221] != 0; result.pic_order_present = p[222] != 0; result.direct8 = p[220] != 0;
    result.deblock = p[225] != 0; result.redundant = p[226] != 0; result.transform8 = (result.flags & 0x2000) != 0;
    // Level 改變 MinLumaBipredSize8x8；不能一律提高 level 而改變 4x4 bi-prediction。
    result.level = (result.flags & 0x4000) ? 52U : 30U;
    const std::uint64_t frame_mbs = std::uint64_t{result.width_mbs} * result.height_mbs;
    const std::uint64_t max_mbs = result.level == 30 ? 1620 : 36864;
    const std::uint64_t max_dpb = result.level == 30 ? 8100 : 184320;
    require(frame_mbs <= max_mbs && frame_mbs * result.refs <= max_dpb &&
            std::uint64_t{result.width_mbs} * result.width_mbs <= max_mbs * 8 &&
            std::uint64_t{result.height_mbs} * result.height_mbs <= max_mbs * 8, "DXVA H.264 coded frame/DPB 超出合成 SPS level 能力");
    return result;
}

std::vector<std::uint8_t> sps(const Parameters& p) {
    Writer w;
    w.put(8, 100); w.put(8, 0); w.put(8, p.level); w.ue(0); // High profile，SPS id 0。
    w.ue(1); w.ue(0); w.ue(0); w.put(1, 0); w.put(1, 0); // 4:2:0/8-bit/no bypass；matrix 在 PPS。
    w.ue(p.frame_bits - 4); w.ue(p.poc_type);
    if (p.poc_type == 0) w.ue(p.poc_bits - 4);
    w.ue(p.refs); w.put(1, 1); // 允許 frame_num gaps；實際 non-existing refs 仍明確拒絕。
    w.ue(p.width_mbs - 1); w.ue(p.height_mbs - 1);
    w.put(1, 1); w.put(1, p.direct8); w.put(1, 0); w.put(1, 0); // progressive，無 crop/VUI。
    return w.nal(0x67);
}

std::vector<std::uint8_t> pps(const Parameters& p, std::uint32_t id, std::span<const std::uint8_t> matrix) {
    Writer w;
    w.ue(id); w.ue(0); w.put(1, p.entropy); w.put(1, p.pic_order_present); w.ue(0);
    w.ue(p.default_l0); w.ue(p.default_l1); w.put(1, (p.flags >> 8) & 1); w.put(2, (p.flags >> 9) & 3);
    w.se(p.qp); w.se(p.qs); w.se(p.chroma);
    w.put(1, p.deblock); w.put(1, (p.flags >> 7) & 1); w.put(1, p.redundant);
    w.put(1, p.transform8); w.put(1, 1);
    std::size_t offset = 0;
    for (unsigned list = 0; list < (p.transform8 ? 8U : 6U); ++list) {
        w.put(1, 1); // 明確寫出每一個 matrix，避免 SPS/PPS fallback 改變量化值。
        int previous = 8;
        for (unsigned j = 0; j < (list < 6 ? 16U : 64U); ++j) {
            const int value = matrix[offset++];
            require(value != 0, "DXVA H.264 scaling list 不能含零");
            const int delta = ((value - previous + 128 + 256) % 256) - 128;
            w.se(delta); previous = value;
        }
    }
    w.se(p.second_chroma);
    return w.nal(0x68);
}
} // namespace

DxvaH264Packet convert_dxva_h264(std::span<const std::uint8_t> picture,
                               std::span<const std::uint8_t> matrix,
                               std::span<const std::uint8_t> slices,
                               std::span<const std::uint8_t> bitstream) {
    const auto p = parameters(picture);
    require(matrix.size() == dxva_h264_matrix_bytes, "DXVA_Qmatrix_H264 必須為 224 bytes");
    require(!slices.empty() && slices.size() % dxva_h264_short_slice_bytes == 0 &&
            slices.size() / dxva_h264_short_slice_bytes <= 65536, "DXVA H.264 short slice table 尺寸無效");
    require(!bitstream.empty() && bitstream.size() <= 64U * 1024U * 1024U, "DXVA H.264 bitstream 尺寸無效");
    DxvaH264Packet result;
    result.coded_width = p.width_mbs * 16; result.coded_height = p.height_mbs * 16;
    require(!(picture[4] & 0x80), "DXVA H.264 CurrPic surface 無效或為 bottom field");
    result.target_surface = picture[4]; result.reference_picture = (p.flags & 0x40) != 0;
    result.frame_num = static_cast<std::uint16_t>(le(picture, 214, 2));
    require(result.frame_num < (std::uint32_t{1} << p.frame_bits), "DXVA H.264 frame_num 超出 SPS 位寬");
    result.field_order_count = {signed_dword(picture, 32), signed_dword(picture, 36)};
    const auto used = le(picture, 208, 4);
    std::array<bool, 128> surfaces{};
    for (unsigned i = 0; i < 16; ++i) {
        const auto entry = picture[16 + i];
        const auto fields = (used >> (2 * i)) & 3;
        if (entry == 0xff) { require(fields == 0, "DXVA H.264 無效 reference surface 仍被引用"); continue; }
        require(fields == 3, "DXVA H.264 progressive reference 必須同時包含兩 field");
        const auto surface = static_cast<std::uint8_t>(entry & 0x7f);
        require(!surfaces[surface] && surface != result.target_surface, "DXVA H.264 reference surface 重複或與目標衝突");
        surfaces[surface] = true;
        const bool long_term = (entry & 0x80) != 0;
        const auto number = le(picture, 176 + 2 * i, 2);
        require(number < (long_term ? p.refs : (std::uint32_t{1} << p.frame_bits)),
                "DXVA H.264 reference frame_num/long-term index 超出 SPS 範圍");
        result.references.push_back({surface, long_term, static_cast<std::uint16_t>(number),
                                     {signed_dword(picture, 40 + 8 * i), signed_dword(picture, 44 + 8 * i)}});
    }
    require(result.references.size() <= p.refs, "DXVA H.264 reference table 超出 SPS max_num_ref_frames");
    std::optional<std::uint32_t> pps_id, previous_mb, idr_id;
    std::optional<bool> idr;
    std::size_t previous_end = 0;
    bool all_intra = true;
    for (std::size_t offset = 0; offset < slices.size(); offset += dxva_h264_short_slice_bytes) {
        const auto start = static_cast<std::size_t>(le(slices, offset, 4));
        auto size = static_cast<std::size_t>(le(slices, offset + 4, 4));
        require(le(slices, offset + 8, 2) == 0, "DXVA H.264 chopped slice 尚未支援");
        require(start >= previous_end && start <= bitstream.size() && size <= bitstream.size() - start, "DXVA H.264 slice 範圍越界或重疊");
        previous_end = start + size;
        auto nal = bitstream.subspan(start, size);
        std::size_t prefix = 0;
        if (nal.size() >= 3 && nal[0] == 0 && nal[1] == 0 && nal[2] == 1) prefix = 3;
        else if (nal.size() >= 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1) prefix = 4;
        require(prefix != 0, "DXVA H.264 slice 缺少 Annex-B start code");
        nal = nal.subspan(prefix);
        // DXVA 最後一 slice 包含 128-byte padding；CABAC zero words 亦非 NAL payload 必要部分。
        while (!nal.empty() && nal.back() == 0) nal = nal.first(nal.size() - 1);
        require(nal.size() >= 2, "DXVA H.264 slice NAL 截斷");
        const auto type = nal[0] & 31U;
        require(type == 1 || type == 5, "DXVA H.264 僅接受完整 AVC slice NAL（非 MVC/SVC/data partition）");
        const bool terminal_epb = nal.size() >= 3 && nal[nal.size() - 3] == 0 &&
                                  nal[nal.size() - 2] == 0 && nal.back() == 3;
        require(!terminal_epb || p.entropy, "DXVA H.264 CAVLC slice 不可包含 CABAC zero word");
        require(((nal[0] & 0x60) != 0) == result.reference_picture, "DXVA H.264 RefPicFlag 與 slice nal_ref_idc 不符");
        const auto payload = rbsp(nal); Bits reader(payload);
        const auto mb = reader.ue(), slice_type = reader.ue(), current_pps = reader.ue();
        require(mb < p.width_mbs * p.height_mbs && (!previous_mb ? mb == 0 : mb > *previous_mb), "DXVA H.264 first_mb 無效或使用 ASO");
        require(slice_type <= 9 && slice_type % 5 <= 2, "DXVA H.264 尚未支援 SP/SI slice");
        require(current_pps <= 255 && (!pps_id || *pps_id == current_pps), "DXVA H.264 slice PPS id 超出範圍或不一致");
        require(reader.read(p.frame_bits) == result.frame_num, "DXVA H.264 slice frame_num 與 picture parameters 不符");
        const bool current_idr = type == 5;
        require(!idr || *idr == current_idr, "DXVA H.264 同一 picture 混用 IDR/non-IDR");
        if (current_idr) {
            const auto current_id = reader.ue();
            require(current_id <= 65535 && (!idr_id || *idr_id == current_id) && result.frame_num == 0 &&
                    result.reference_picture && slice_type % 5 == 2, "DXVA H.264 IDR header 無效或不一致");
            idr_id = current_id;
        }
        if (p.poc_type == 0) {
            const auto lsb = reader.read(p.poc_bits);
            const auto mask = (std::uint32_t{1} << p.poc_bits) - 1;
            require((static_cast<std::uint32_t>(result.field_order_count[0]) & mask) == lsb, "DXVA H.264 slice POC 與 picture parameters 不符");
            const auto delta = p.pic_order_present ? reader.se() : 0;
            require(std::int64_t{result.field_order_count[1]} - result.field_order_count[0] == delta, "DXVA H.264 bottom POC 差值不符");
        }
        pps_id = current_pps; previous_mb = mb; idr = current_idr;
        all_intra = all_intra && slice_type % 5 == 2;
        size = nal.size();
        require(size <= 64U * 1024U * 1024U - 4 &&
                result.sample.size() <= 64U * 1024U * 1024U - size - 4, "DXVA H.264 AVCC sample 超出上限");
        for (unsigned shift : {24U, 16U, 8U, 0U}) result.sample.push_back(static_cast<std::uint8_t>(size >> shift));
        result.sample.insert(result.sample.end(), nal.begin(), nal.end());
    }
    require(all_intra == ((p.flags & 0x8000) != 0), "DXVA H.264 IntraPicFlag 與 slice types 不符");
    result.idr = *idr; result.picture_parameter_set_id = *pps_id;
    require(!result.idr || result.references.empty(), "DXVA H.264 IDR 不應引用先前 surfaces");
    result.parameter_sets = {sps(p), pps(p, *pps_id, matrix)};
    return result;
}
} // namespace x64arm::video
