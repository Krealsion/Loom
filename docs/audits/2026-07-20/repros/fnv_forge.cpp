#include <cstdint>
#include <cstdio>
#include <string>
#include <array>
static std::uint64_t fnv1a64(const std::string& b){std::uint64_t h=1469598103934665603ULL;for(char ch:b){h^=(unsigned char)ch;h*=1099511628211ULL;}return h;}
int main(){
 std::string benign=std::string("BENIGN-STORAGE-MOD v1.0 (blessed with network+fs)")+ '\0' + "payload...";
 std::uint64_t T=fnv1a64(benign);
 std::string mal="MALICIOUS-MOD: rm -rf ~; exfiltrate(); // never blessed";
 const int kSuf=8;
 auto hs=[&](std::uint64_t bits){std::string m=mal;for(int i=0;i<kSuf;++i)m.push_back((char)((bits>>(8*i))&0xFF));return fnv1a64(m);};
 std::uint64_t c=hs(0); std::array<std::uint64_t,64> A{};
 for(int i=0;i<64;++i)A[i]=hs(1ULL<<i)^c;
 std::uint64_t rhs=T^c; std::array<std::uint64_t,64> R{}; std::array<int,64> Rb{};
 for(int r=0;r<64;++r){std::uint64_t co=0;for(int i=0;i<64;++i)if((A[i]>>r)&1ULL)co|=(1ULL<<i);R[r]=co;Rb[r]=(int)((rhs>>r)&1ULL);}
 std::array<int,64> pc; pc.fill(-1); int rank=0;
 for(int col=0;col<64&&rank<64;++col){int piv=-1;for(int r=rank;r<64;++r)if((R[r]>>col)&1ULL){piv=r;break;}if(piv<0)continue;std::swap(R[piv],R[rank]);std::swap(Rb[piv],Rb[rank]);for(int r=0;r<64;++r)if(r!=rank&&((R[r]>>col)&1ULL)){R[r]^=R[rank];Rb[r]^=Rb[rank];}pc[rank]=col;++rank;}
 std::uint64_t x=0; for(int r=0;r<rank;++r)if(pc[r]>=0&&Rb[r])x|=(1ULL<<pc[r]);
 std::string forged=mal; for(int i=0;i<kSuf;++i)forged.push_back((char)((x>>(8*i))&0xFF));
 std::uint64_t H=fnv1a64(forged);
 printf("benign hash T = %016llx (len %zu)\n",(unsigned long long)T,benign.size());
 printf("forged hash   = %016llx (len %zu)\n",(unsigned long long)H,forged.size());
 printf("rank=%d  bytes_equal=%s  COLLIDE=%s\n",rank, benign==forged?"YES":"no", H==T?"YES-second-preimage":"NO");
 return H==T?0:1;}
