/* beamformer.hh 

	Contains all functions and defines UNrelated to CUDA

*/

#include <iostream>
#include <sstream>
#include <iomanip>
#include <string>
#include <cmath>
#include <fstream>
#include <cstdint>
#include <unistd.h> //optarg



/* dada includes */
#ifndef DEBUG
	#include <algorithm>
	#include <stdlib.h>
	#include <math.h>
	#include <string.h>
	#include <netdb.h>
	#include <sys/socket.h>
	#include <sys/types.h>
	#include <netinet/in.h>
	#include <time.h>

	#include "dada_client.h"
	#include "dada_def.h"
	#include "dada_hdu.h"
	#include "multilog.h"
	#include "ipcio.h"
	#include "ipcbuf.h"
	#include "dada_affinity.h"
	#include "ascii_header.h"
#endif


/***************************************************
				Configuration 
***************************************************/

#if DEBUG
	/* If 1 simulates a point source which varies across the field of view
	   if 0 Bogus data is used instead */
	#define GENERATE_TEST_DATA 1
	#define BOGUS_DATA 0x70
#endif

/***************************************************
			    DSA Constants
***************************************************/

#define N_BEAMS 256
#define N_ANTENNAS 64
#define N_FREQUENCIES 256
#define HALF_FOV 3.5

#define N_POL 2				//Number of polarizations
#define N_CX 2				//Number of real numbers in a complex number, namely 2

#if DEBUG
	/* Number of time samples to average after beamforming */
	#define N_AVERAGING 1
#else
	#define N_AVERAGING 16
#endif


/***************************************************
				DATA constants
***************************************************/

/* How many matrix multiplications could be executed based on the amount of data on the GPU*/
#define N_GEMMS_PER_GPU 256

/* How many output tensors are generated by each GEMM. This parameter helps improve throughput*/
#define N_OUTPUTS_PER_GEMM 8

/* Based on the size of a dada blocks: How many matrix-matrix multiplacations are need */
#define N_GEMMS_PER_BLOCK 64

/* For each output, we need to average over 16 iterations and 2 polarizations*/
#define N_INPUTS_PER_OUTPUT (N_POL*N_AVERAGING)

/* This is the number of columns processed in each matrix multiplication (includes 2 pol)*/
#define N_TIMESTEPS_PER_GEMM (N_OUTPUTS_PER_GEMM*N_INPUTS_PER_OUTPUT)

/* Calculates the number of blocks on the GPU given the number of GEMMMs possible on the GPU
   and the number of gemms contained in each block*/
#define N_BLOCKS_on_GPU (N_GEMMS_PER_GPU/N_GEMMS_PER_BLOCK)

/* Number of complex numbers of input data are needed for each GEMM */
#define N_CX_IN_PER_GEMM  (N_ANTENNAS*N_FREQUENCIES*N_TIMESTEPS_PER_GEMM)

/* Number of Complex numbers of output data are produced in each GEMM */
#define N_CX_OUT_PER_GEMM (N_BEAMS*N_FREQUENCIES*N_TIMESTEPS_PER_GEMM)

/* The detection step averages over N_INPUTS_PER_OUTPUT (16) numbers */
#define N_F_PER_DETECT (N_CX_OUT_PER_GEMM/N_INPUTS_PER_OUTPUT)

/* Number of Bytes of input data are needed for each GEMM, the real part and imaginary parts
   of each complex number use 1 Byte after expansion */
#define N_BYTES_POST_EXPANSION_PER_GEMM  (N_CX_IN_PER_GEMM*N_CX)

/* Number of Bytes before expansion. Each complex number uses half a Byte */
#define N_BYTES_PRE_EXPANSION_PER_GEMM  N_CX_IN_PER_GEMM*N_CX/2

/* Number of Bytes (before expansion) for input array */
#define N_BYTES_PER_BLOCK N_BYTES_PRE_EXPANSION_PER_GEMM*N_GEMMS_PER_BLOCK


#define INPUT_DATA_SIZE N_BYTES_PRE_EXPANSION_PER_GEMM*N_DIRS

// Data Indexing, Offsets
#define N_GPUS 8
#define TOT_CHANNELS 2048
#define START_F 1.28
#define END_F 1.53
#define ZERO_PT 0
#define BW_PER_CHANNEL ((END_F - START_F)/TOT_CHANNELS)

// Numerical Constants
#define C_SPEED 299792458.0
#define PI 3.14159265358979


// Type Constants
#define N_BITS 8
#define MAX_VAL 127

#define SIG_BITS 4
#define SIG_MAX_VAL 7

// Solving Constants
#define N_STREAMS 8
#define N_DIRS  1024
#define MAX_TRANSFER_SEP 2
#define MAX_TOTAL_SEP 4


/***************************************************
				TYPES
***************************************************/

typedef char2 CxInt8_t;
typedef char char4_t[4]; //four chars = 32-bit so global memory bandwidth usage is optimal
typedef char char8_t[8]; //eight chars = 64-bit so global memory bandwidth usage is optimal
typedef CxInt8_t cuChar4_t[4];

class antenna{
/* Class which manages the x, y, and z positions of each antenna */
public:
	float x, y, z;
	antenna(){x = 0; y = 0; z = 0;}
	~antenna(){}
};

class beam_direction{
/* Class whcih manages the theta and phi directions of each beam */
public:
	float theta, phi;
	beam_direction(){theta = 0; phi = 0;}
	~beam_direction(){}
};

/***************************************************
				DEFINED FUNCTIONS
***************************************************/

/* Macro which converts from degrees to radians */
#define DEG2RAD(x) ((x)*PI/180.0)

/***************************************************
				DADA
***************************************************/


/* Usage as defined by dada example code */
#if DEBUG
void usage(){
	fprintf (stdout,
	   "dsaX_beamformer [options]\n"
	   " -g gpu 	select a predefined frequency range\n"
	   " -f position_filename  file where the antenna positions are stored\n"
	   " -d direction_filename file where the beam directions are stored\n"
	   " -h         print usage\n");
}
#else
void usage(){
	fprintf (stdout,
	   "dsaX_beamformer [options]\n"
	   " -c core    bind process to CPU core\n"
	   " -k key     [default dada]\n"
	   " -g gpu 	select a predefined frequency range\n"
	   " -f position_filename  file where the antenna positions are stored\n"
	   " -d direction_filename file where the beam directions are stored\n"
	   " -h         print usage\n");
}
#endif

#ifndef DEBUG
/*cleanup as defined by dada example code */
void dsaX_dbgpu_cleanup (dada_hdu_t * in,  multilog_t * log) {
	if (dada_hdu_unlock_read (in) < 0){
		multilog(log, LOG_ERR, "could not unlock read on hdu_in\n");
	}
	dada_hdu_destroy (in);
}
#endif


/***************************************************
				UTILITY FUNCTIONS
***************************************************/

#if DEBUG
void generate_test_data(char *data, antenna pos[], beam_direction dir[], int gpu, int stride){
	// float test_direction;
	char high, low;
	
	for (long direction = 0; direction < N_DIRS; direction++){
		// test_direction = dir[direction];   //DEG2RAD(-HALF_FOV) + ((float) direction)*DEG2RAD(2*HALF_FOV)/(N_DIRS-1);
		for (int i = 0; i < N_FREQUENCIES; i++){
			float freq = END_F - (ZERO_PT + gpu*TOT_CHANNELS/(N_GPUS-1) + i)*BW_PER_CHANNEL;
			// std::cout << "freq: " << freq << std::endl;
			float wavelength = C_SPEED/(1E9*freq);
			for (int j = 0; j < N_TIMESTEPS_PER_GEMM; j++){
				for (int k = 0; k < N_ANTENNAS; k++){

					high = ((char) round(SIG_MAX_VAL*cos(2*PI*(pos[k].x*sin(dir[direction].x) + pos[k].y*sin(dir[direction].phi))/wavelength))); //real
					low  = ((char) round(SIG_MAX_VAL*sin(2*PI*(pos[k].x*sin(dir[direction].x) + pos[k].y*sin(dir[direction].phi))/wavelength))); //imag

					data[direction*N_BYTES_PRE_EXPANSION_PER_GEMM + i*stride + j*N_ANTENNAS + k] = (high << 4) | (0x0F & low);
				}
			}
		}
	}
}
#endif

int read_in_beam_directions(char * file_name, beam_direction* dir, bool * dir_set){
	std::ifstream input_file;

	input_file.open(file_name);
	int nbeam;
	input_file >> nbeam;
	if (nbeam != N_BEAMS){
		std::cout << "Number of beams in file (" << nbeam << ") does not match N_BEAMS ("<< N_BEAMS << ")" <<std::endl;
		std::cout << "Excess beams will be ignored, missing beams will be set to 0." << std::endl;
	}

	for (int beam_idx = 0; beam_idx < N_BEAMS; beam_idx++){
		input_file >> dir[beam_idx].theta >> dir[beam_idx].phi;
		//std::cout << "Read in: (" << dir[beam_idx].theta << ", " << dir[beam_idx].phi << ")" << std::endl;
	}
	*dir_set = true;
	return 0;
}


int read_in_position_locations(char * file_name, antenna *pos, bool *pos_set){
	std::ifstream input_file;
	input_file.open(file_name);
	int nant;
	input_file >> nant;
	if (nant != N_ANTENNAS){
		std::cout << "Number of antennas in file (" << nant << ") does not match N_ANTENNAS ("<< N_ANTENNAS << ")" <<std::endl;
		std::cout << "Excess antennas will be ignored, missing antennas will be set to 0." << std::endl;
	}

	for (int ant = 0; ant < N_ANTENNAS; ant++){
		input_file >> pos[ant].x >> pos[ant].y >> pos[ant].z;
		//std::cout << "Read in: (" << pos[ant].x << ", " << pos[ant].y << ", " << pos[ant].z << ")" << std::endl;
	}
	*pos_set = true;
	return 0;
}


void write_array_to_disk_as_python_file(float *data_out, int rows, int cols, char * output_filename){
	/* Export debug data to a python file. */

	std::ofstream f; // File for data output
	f.open(output_filename); // written such that it can be imported into any python file
	f << "A = [[";
	
	for (int jj = 0; jj < rows; jj++){
		for (int ii = 0; ii < cols; ii++){
			f << data_out[jj*cols + ii];
			// std::cout << data_out[jj*cols + ii] << ", ";
			if (ii != cols - 1){
				f << ",";
			}
		}

		if (jj != rows-1){
			f << "],\n[";
		} else {
			f<< "]]"<<std::endl;
		}
	}

	f.close();
}


void print_all_defines(void){
	std::cout << "N_BEAMS:" << N_BEAMS << "\n";
	std::cout << "N_ANTENNAS:" << N_ANTENNAS << "\n";
	std::cout << "N_FREQUENCIES:" << N_FREQUENCIES << "\n";
	std::cout << "N_AVERAGING:" << N_AVERAGING << "\n";
	std::cout << "N_POL:" << N_POL << "\n";
	std::cout << "N_CX:" << N_CX << "\n";
	std::cout << "N_GEMMS_PER_GPU:" << N_GEMMS_PER_GPU << "\n";
	std::cout << "N_OUTPUTS_PER_GEMM:" << N_OUTPUTS_PER_GEMM << "\n";
	std::cout << "N_GEMMS_PER_BLOCK:" << N_GEMMS_PER_BLOCK << "\n";
	std::cout << "N_INPUTS_PER_OUTPUT:" << N_INPUTS_PER_OUTPUT << "\n";
	std::cout << "N_TIMESTEPS_PER_GEMM:" << N_TIMESTEPS_PER_GEMM << "\n";
	std::cout << "N_BLOCKS_on_GPU:" << N_BLOCKS_on_GPU << "\n";
	std::cout << "N_CX_IN_PER_GEMM:" << N_CX_IN_PER_GEMM << "\n";
	std::cout << "N_CX_OUT_PER_GEMM:" << N_CX_OUT_PER_GEMM << "\n";
	std::cout << "N_BYTES_POST_EXPANSION_PER_GEMM:" << N_BYTES_POST_EXPANSION_PER_GEMM << "\n";
	std::cout << "N_BYTES_PRE_EXPANSION_PER_GEMM:" << N_BYTES_PRE_EXPANSION_PER_GEMM << "\n";
	std::cout << "N_BYTES_PER_BLOCK:" << N_BYTES_PER_BLOCK << "\n";
	std::cout << "N_GPUS:" << N_GPUS << "\n";
	std::cout << "TOT_CHANNELS:" << TOT_CHANNELS << "\n";
	std::cout << "START_F:" << START_F << "\n";
	std::cout << "END_F:" << END_F << "\n";
	std::cout << "ZERO_PT:" << ZERO_PT << "\n";
	std::cout << "BW_PER_CHANNEL:" << BW_PER_CHANNEL << "\n";
	std::cout << "C_SPEED:" << C_SPEED << "\n";
	std::cout << "PI:" << PI <<"\n";
	std::cout << "N_BITS:" << N_BITS << "\n";
	std::cout << "MAX_VAL:" << MAX_VAL << "\n";
	std::cout << "SIG_BITS:" << SIG_BITS << "\n";
	std::cout << "SIG_MAX_VAL:" << SIG_MAX_VAL << "\n";
	std::cout << "N_STREAMS:" << N_STREAMS << "\n";
	std::cout << "N_DIRS:" << N_DIRS << "\n";

	std::cout << std::endl;
}









