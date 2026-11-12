#include "fh3mod.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
int files=0,labels=0;bool sql=false,version=false,updates=false;std::filesystem::path output;
FH3Mod* register_mod(const FH3ModInfo* info){version=std::strcmp(info->version,"1.0.0")==0;updates=info->name&&std::strcmp(info->name,"Car Meets")==0&&info->github_repo&&std::strcmp(info->github_repo,"Laur1sXd/FH3-Car-Meets")==0&&info->check_updates==1&&info->platform_version&&std::strcmp(info->platform_version,"1.3.0")==0;return reinterpret_cast<FH3Mod*>(1);}
void log(FH3Mod*,const char* s){std::puts(s);}
int set_file(FH3Mod*,const char* name,const void* data,size_t size){++files;auto path=output/name;std::filesystem::create_directories(path.parent_path());std::ofstream f(path,std::ios::binary);f.write(static_cast<const char*>(data),size);return f.good();}
void clear_file(FH3Mod*,const char*){}
void add_string(FH3Mod*,const char* table,uint32_t,const wchar_t* value){if(std::strcmp(table,"CarMeets.str")==0&&std::wcslen(value)>0)++labels;}
void add_sql(FH3Mod*,const char* s){sql=std::strcmp(s,"INSERT INTO CarMeets (Id, RoadTripLocationId) VALUES (101,9),(102,9),(104,9),(105,9),(106,9),(107,9),(108,9),(109,9);")==0;}
void* wait_for_code(uint32_t){return nullptr;}
int wmain(int argc,wchar_t** argv){
 if(argc!=3)return 2;
 auto scratch=std::filesystem::path(argv[2]);
 if(std::filesystem::exists(scratch/"fh3-mods/CarMeetLocations.dat"))return 6;
 output=scratch/"captured";
 auto dll=LoadLibraryW(argv[1]);if(!dll)return 3;
 auto init=reinterpret_cast<int (*)(const FH3ModApi*)>(GetProcAddress(dll,"fh3mod_init"));if(!init)return 4;
 FH3ModApi api{};api.size=sizeof(api);api.api_version=FH3MOD_API_VERSION;api.azimuth_dir=argv[2];
 api.register_mod=register_mod;api.log=log;api.set_file=set_file;api.clear_file=clear_file;api.add_string_key=add_string;api.add_sql=add_sql;api.wait_for_code=wait_for_code;
 init(&api);Sleep(50);
 std::printf("Standalone DLL: %d files, %d labels, version %s, SQL %s; no external DAT.\n",files,labels,version?"1.0.0":"wrong",sql?"correct":"wrong");
 std::printf("MODS name and native GitHub update registration: %s.\n",updates?"correct":"wrong");
 return files==23&&labels==16&&version&&sql&&updates?0:5;
}
