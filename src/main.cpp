#include <iostream>
#include <iomanip>
#include <fstream>
#include <cstring>

#include <tclap/CmdLine.h>
#include <tclap/ValueArg.h>

#include "diff.h"
#include "derand.h"
#include "imagebuilder.h"
#include "simpledeframer.h"

#define BUFFER_SIZE 1024
#define FRAME_SIZE_BITS 209200
#define FRAME_SIZE_BYTES (FRAME_SIZE_BITS/8)

bool decodeFile(std::string filename, bool debug, std::string prefix, bool resize, bool despeckle);
size_t get_filesize(std::string filename);

static struct {
    std::string name[22];
} productID = {{
    "No Data",
    "AAA IR Data",
    "AAA Visible Data",
    "GVAR Imager Documentation", 
    "GVAR Imager IR Data",
    "GVAR Imager Visible Data",
    "GVAR Sounder Documentation",
    "GVAR Sounder Scan Data",
    "GVAR Compensation Data",
    "GVAR Telemetry Statistics",
    "Not Used",
    "GIMTACS Text",
    "AAA Sounding Products",
    "GVAR ECAL Data",
    "GVAR Spacelook Data",
    "GVAR BB Data",
    "GVAR Calibration Coefficients",
    "GVAR Visible NLUTs",
    "GVAR Star Sense Data",
    "GVAR Imager Factory Coefficients",
    "Unassigned",
    "Invalid Product ID"
}};

int main(int argc, char **argv) {
    TCLAP::CmdLine cmd("GVAR tools - a set of (crappy) tools for dealing with GVAR data - image decoder", ' ', "0.1");

    TCLAP::UnlabeledValueArg<std::string> binPath("name", "path to a .bin file containing GVAR data", true, "", "path");
    cmd.add(binPath);
    TCLAP::SwitchArg debugSwitch("d", "debug", "Print debug information, warning: spams the console", false);
    cmd.add(debugSwitch);
    TCLAP::SwitchArg resizeSwitch("r", "dont-resize", "Don't resize the output channels to 1:1", false);
    cmd.add(resizeSwitch);
    TCLAP::SwitchArg despeckleSwitch("s", "dont-despeckle", "Don't despeckle the channels", false);
    cmd.add(despeckleSwitch);
    TCLAP::ValueArg<std::string> prefixValue("p", "prefix", "prefix of the output image filenames", false, "G13", "");
    cmd.add(prefixValue);

    try{
        cmd.parse(argc, argv);
    } catch (TCLAP::ArgException &e) {
        std::cerr << "Error: " << e.error() << " for arg " << e.argId() << std::endl;
    }

    if(!decodeFile(binPath.getValue(), debugSwitch.getValue(), prefixValue.getValue(), !resizeSwitch.getValue(), !despeckleSwitch.getValue())){
        return 1;
    }

    return 0;
}

bool decodeFile(std::string filename, bool debug, std::string prefix, bool resize, bool despeckle){
    std::ifstream data_in(filename, std::ios::binary);

    if(!data_in){
        std::cout << "Error: could not open input file" << std::endl;
        return false;
    }

    size_t filesize = get_filesize(filename);

    uint8_t *buffer = new uint8_t[BUFFER_SIZE];
    uint8_t *frame = new uint8_t[FRAME_SIZE_BYTES];

    DifferentialEncoding diffDecoder;
    SimpleDeframer<uint64_t, 64, FRAME_SIZE_BITS, 0b0001101111100111110100000001111110111111100000001111111111111110> deframer;
    GVARDerandomizer derand(FRAME_SIZE_BITS, 8, 0b101001110110101);

    // Image builders
    ImageBuilder channels[5] = {
        ImageBuilder(5000, 6000),
        ImageBuilder(5000, 6000),
        ImageBuilder(5000, 6000),
        ImageBuilder(5000, 6000),
        ImageBuilder(20000, 24000)
    };

    // Read until EOF
    unsigned int frames = 0;
    uint16_t lastIFRAM[3] = { 0 };
    while (!data_in.eof()) {
        data_in.read((char *)buffer, BUFFER_SIZE);

        diffDecoder.nrzsDecode(buffer, BUFFER_SIZE);

        // Deframe
        // TODO: change output to write directly to a uint8_t pointer
        std::vector<std::vector<uint8_t>> deframedBuffer = deframer.work(buffer, 1024);
        if(deframedBuffer.size() == 0) continue;
        std::memcpy(frame, &deframedBuffer[0][0], deframedBuffer[0].size());

        derand.work(frame);

        // BEGIN: header parsing
        uint8_t block = frame[8];
        if(block == 254) block = 0;
        if(block > 11 && block != 15) block = 0;

        uint8_t wordSize = frame[9];
        if(wordSize != 6 && wordSize != 8 && wordSize != 10) wordSize = 0;

        uint16_t product    = ((uint16_t)frame[12] << 8) | frame[13];
        if(product > 1000) product = 22;
        if(product > 20) product = 21;

        uint16_t IFRAM      = ((uint16_t)frame[105] << 8) | frame[106];
        IFRAM /= 4;

        uint16_t wordCount  = ((uint16_t)frame[10] << 8) | frame[11];
        uint16_t blockCount = ((uint16_t)frame[20] << 8) | frame[21];
        // END: header parsing

        // These could of been done with std::cout but would become messy quickly
        if(debug){
            printf("Block ID: %3i, Product ID: %2i, Block count: %5i, Word count: %5i, Word size: %2i, Frame counter: %3i\r\n",
                block,
                product,
                blockCount,
                wordCount,
                wordSize,
                IFRAM
            );
        }else{
            printf("\rProgress: %4.1f%%, Frame counter: %5i, Total (good) frames: %5i, Product: %-32s",
                (float)data_in.tellg() / (float)filesize * 100.0f,
                blockCount,
                frames,
                productID.name[product].c_str()
            );
            fflush(stdout);
        }

        // Channels 1 and 2
        if(block == 1){
            if(IFRAM - lastIFRAM[0] > 1 && IFRAM - lastIFRAM[0] < 5 && lastIFRAM[0] != 63 && lastIFRAM[0] != 0){
                // Multiply by two since there are 2 lines per frame
                channels[0].cloneLastRow((IFRAM - lastIFRAM[0] - 1) * 2);
                channels[1].cloneLastRow((IFRAM - lastIFRAM[0] - 1) * 2);
            }

            channels[0].pushRow(&frame[120], 6250, 0);
            channels[0].pushRow(&frame[6250 + 280 + 120], 6250, 2);

            channels[1].pushRow(&frame[6250*2 + 280*2 + 120], 6250, 0);
            channels[1].pushRow(&frame[6250*3 + 280*3 + 120], 6250, 2);

            lastIFRAM[0] = IFRAM;
        // Channels 3 and 4
        }else if(block == 2){
            if(IFRAM - lastIFRAM[1] > 1 && IFRAM - lastIFRAM[1] < 5 && lastIFRAM[1] != 63 && lastIFRAM[1] != 0){
                // Multiply by two since there are 2 lines per frame
                channels[2].cloneLastRow((IFRAM - lastIFRAM[1] - 1) * 2);
                channels[3].cloneLastRow((IFRAM - lastIFRAM[1] - 1) * 2);
            }

            channels[2].pushRow(&frame[120], 6250, 0);
            channels[2].pushRow(&frame[6250 + 280 + 120], 6250, 2);

            channels[3].pushRow(&frame[6250*2 + 280*2 + 120], 6250, 0);
            channels[3].pushRow(&frame[6250*2 + 280*2 + 120], 6250, 0);

            lastIFRAM[1] = IFRAM;
        // Channel 5 (aka thicc boi)
        }else if(block >= 3 && block <= 10){
            if(IFRAM - lastIFRAM[2] > 1 && IFRAM - lastIFRAM[2] < 5 && lastIFRAM[2] != 63 && lastIFRAM[2] != 0){
                channels[4].cloneLastRow(IFRAM - lastIFRAM[2] - 1);
            }

            channels[4].pushRow(&frame[120], 25000, 0);

            lastIFRAM[2] = IFRAM;
        }
        
        frames++;
    }

    std::cout << std::endl << "Total frames: " << frames << std::endl;

    if(despeckle){
        for(int ch = 0; ch < 5; ch++) channels[ch].despeckle();
    }

    for(int ch = 0; ch < 5; ch++) {
        std::cout << "Writing channel..." << ch+1 << std::endl;
        channels[ch].saveImage(prefix + "-" + std::to_string(ch+1) + ".png", resize);
    }

    // Clean up
    data_in.close();
    return true;
}

size_t get_filesize(std::string filename) {
    std::ifstream in(filename, std::ifstream::ate | std::ifstream::binary);
    size_t filesize = in.tellg();
    in.close();
    return filesize;
}
