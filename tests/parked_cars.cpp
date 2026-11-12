#include "../src/plugin.cpp"
#include <cassert>
#include <iostream>
#include <set>

namespace {
struct Physics { void** methods; Position point; };
const Position* position(void* object, Position* out) { *out = static_cast<Physics*>(object)->point; return out; }
template<class T> void put(std::vector<std::byte>& bytes, size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
struct Fixture {
    std::vector<std::byte> slot = std::vector<std::byte>(0x110);
    std::vector<std::byte> participant = std::vector<std::byte>(0x80);
    std::vector<std::byte> car = std::vector<std::byte>(0x7400);
    std::vector<std::byte> presentation = std::vector<std::byte>(0x300);
    std::array<void*, 0x310/8> methods{};
    Physics physics{methods.data(), {}};
    void setup(unsigned index, const Site& site, bool model_loaded = true) {
        methods[0x308/8] = reinterpret_cast<void*>(position);
        physics.point = site.world(aligned_bay(index));
        put(slot, 0xf0, index); put(slot, 0xc8, participant.data());
        put(participant, 0x50, uint32_t{3}); put(participant, 0x58, car.data());
        put(car, 0x71e8, &physics);
        put(car, 0x73a0, model_loaded ? presentation.data() : nullptr);
        put(presentation, 0, grounding.game + 0x468e848);
        put(presentation, 0x1d0, physics.point);
        remember_parked_car(slot.data());
    }
    int query(FH3MeetAppearance& out) {
        const float point[]{physics.point.x,physics.point.y,physics.point.z};
        return parked_interface.lookup(presentation.data(), point, &out);
    }
};
}
int main() {
    FH3ModApi api{};
    api.set_file = +[](FH3Mod*,const char*,const void*,size_t){return 1;};
    api.add_string_key = +[](FH3Mod*,const char*,uint32_t,const wchar_t*){};
    api.add_sql = +[](FH3Mod*,const char*){};
    api.log = +[](FH3Mod*,const char*){};
    api.wait_for_code = +[](uint32_t)->void*{return nullptr;};
    apply_embedded_payload(&api,reinterpret_cast<FH3Mod*>(1));
    Sleep(50);
    grounding.game = reinterpret_cast<std::byte*>(0x140000000ull);
    assert(grounding.sites.size()==8);
    std::array<Fixture,12> cars;
    for(const auto& site: grounding.sites) {
        parked_members={};
        for(unsigned i=0;i<12;++i) cars[i].setup(i,site);
        std::set<unsigned> colors;
        for(unsigned i=0;i<12;++i) {
            FH3MeetAppearance a{sizeof(a),0,0,0,0,{0,0,0}},b{sizeof(b),0,0,0,0,{0,0,0}};
            assert(cars[i].query(a) && a.slot==i && a.meet==site.id && a.identity);
            remember_parked_car(cars[i].slot.data());
            assert(cars[i].query(b) && std::memcmp(&a,&b,sizeof(a))==0);
            assert(*std::max_element(a.rgb,a.rgb+3) > .99f);
            for(float channel:a.rgb) assert(channel>=.119f && channel<=1);
            colors.insert((unsigned(a.rgb[0]*255)<<16)|(unsigned(a.rgb[1]*255)<<8)|unsigned(a.rgb[2]*255));
        }
        assert(colors.size()>=10);
        FH3MeetAppearance a{sizeof(a),0,0,0,0,{0,0,0}};
        put(cars[0].participant,0x50,uint32_t{0}); assert(!cars[0].query(a));
        put(cars[0].participant,0x50,uint32_t{3}); assert(cars[0].query(a));
        cars[0].physics.point.x+=30; assert(!cars[0].query(a)); cars[0].physics.point.x-=30;
        put(cars[0].participant,0x58,static_cast<std::byte*>(nullptr)); assert(!cars[0].query(a));
        put(cars[0].participant,0x58,cars[0].car.data());
        cars[0].setup(0,site,false); assert(!cars[0].query(a));
        put(cars[0].car,0x73a0,cars[0].presentation.data()); assert(cars[0].query(a));
        const float unrelated[]{0,0,0}; assert(!parked_appearance(cars[0].presentation.data(),unrelated,&a));
        const float invalid[]{NAN,0,0}; assert(!parked_appearance(cars[0].presentation.data(),invalid,&a));
        a.size=0; assert(!cars[0].query(a));
    }
    std::cout<<"PASS: all 8 meets x 12 bays; stable random colors; player/NPC parity; late models; stale ownership and non-meet rejection.\n";
}
