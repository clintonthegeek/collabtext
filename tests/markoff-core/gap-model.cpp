// Independent terminal-interval model. Runtime checks stay active in Release.
#include <map>
#include <stdexcept>
#include <vector>
struct Gap { unsigned actor, first, end, prior; bool operator==(const Gap&) const = default; };
struct Model {
 std::map<unsigned,unsigned> covered;
 std::vector<Gap> terminal;
 int gap(Gap g) {
  if (!g.actor || !g.first || g.first>=g.end || g.prior>=g.first) return -1;
  for(auto old:terminal) if(old.actor==g.actor && g.first<old.end && old.first<g.end) return old==g ? 0 : -1;
  if(covered[g.actor]>=g.first) return -1;
  if(covered[g.actor]<g.prior) return 2;
  terminal.push_back(g);covered[g.actor]=g.end-1;return 1;
 }
 int op(unsigned actor,unsigned first,unsigned last,unsigned prior) {
  for(auto g:terminal) if(g.actor==actor && first<g.end && g.first<=last) return -1;
  if(covered[actor]>=last)return 0;
  if(covered[actor]<prior)return 2;
  covered[actor]=last;return 1;
 }
};
void require(bool x){if(!x)throw std::runtime_error("terminal interval model failed");}
int main(){
 for(unsigned unused=1;unused<100;++unused){
  Model writer,peer;Gap g{1,1,unused+1,0};
  require(peer.op(1,g.end,g.end+1,unused)==2);
  require(writer.gap(g)==1 && peer.gap(g)==1);
  require(writer.op(1,g.end,g.end+1,unused)==1 && peer.op(1,g.end,g.end+1,unused)==1);
  auto reopened=peer;require(reopened.gap(g)==0);
  require(reopened.gap({1,1,unused+1,1})==-1);
  require(reopened.op(1,1,unused,0)==-1);
  require(reopened.gap({1,unused,unused+2,0})==-1);
  require(reopened.covered==writer.covered);
 }
}
