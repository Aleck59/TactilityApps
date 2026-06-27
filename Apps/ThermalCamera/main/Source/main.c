#include "ThermalCamera.h"
#include <TactilityCpp/App.h>

extern "C" {

int main(int argc, char* argv[]) {
    registerApp<ThermalCamera>();
    return 0;
}

}
