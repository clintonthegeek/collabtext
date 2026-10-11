#include "../../libs/collabtext/tests/reference/FugueMaxModel.h"
#include <stdexcept>
void check(bool value) { if (!value) throw std::runtime_error("marker reference model failed"); }
#include <tuple>
int main() {
 using namespace FugueReference;
 auto less=[](const Id&a,const Id&b){return std::tie(a.counter,a.actor)<std::tie(b.counter,b.actor);};
 Tree origin(less), peer(less), first(less), second(less);
 auto initial=origin.insert(0,{"a","b"},{"seed",1});
 for(auto m:initial){peer.deliver(m);first.deliver(m);second.deliver(m);}
 auto marker=origin.insert(1,{""},{"marker",4});
 for(auto m:marker){peer.deliver(m);first.deliver(m);second.deliver(m);}
 auto before=origin.insert(1,{"😀"},{"left",6});
 auto after=peer.insert(2,{"é"},{"right",6});
 for(auto m:before)first.deliver(m);
 for(auto m:after)first.deliver(m);
 for(auto m:after)second.deliver(m);
 for(auto m:before)second.deliver(m);
 check(first.traverse_all()==second.traverse_all());
 first.remove(initial[0].id);first.remove(initial[1].id);
 second.remove(initial[1].id);second.remove(initial[0].id);
 check(first.visible_at(1)==marker[0].id);
 check(second.visible_at(1)==marker[0].id);
 first.remove(marker[0].id);first.deliver(marker[0]);
 check(first.visible_at(1)==after[0].id); // Duplicate delivery cannot revive.
}
