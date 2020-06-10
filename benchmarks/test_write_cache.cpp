#include "hdf5.h"
#include "mpi.h"
#include "stdlib.h"
#include "stdio.h"
#include <sys/time.h>
#include <string.h>
#include "timing.h"
#include <stdlib.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "H5Dio_cache.h"
#include "mpi.h"
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/statvfs.h>
#include <stdlib.h>
#include "stat.h"
#include "debug.h"
#include <unistd.h>

int msleep(long miliseconds)
{
  struct timespec req, rem;

  if(miliseconds > 999)
    {   
      req.tv_sec = (int)(miliseconds / 1000);                            /* Must be Non-Negative */
      req.tv_nsec = (miliseconds - ((long)req.tv_sec * 1000)) * 1000000; /* Must be in range of 0 to 999999999 */
    }   
  else
    {   
      req.tv_sec = 0;                         /* Must be Non-Negative */
      req.tv_nsec = miliseconds * 1000000;    /* Must be in range of 0 to 999999999 */
    }   
  return nanosleep(&req , &rem);
}

int main(int argc, char **argv) {
  char ssd_cache [255] = "no";
  if (getenv("SSD_CACHE")) {
    strcpy(ssd_cache, getenv("SSD_CACHE"));
  }
  bool cache = false; 
  if (strcmp(ssd_cache, "yes")==0) {
    cache=true;
  }

  // Assuming that the dataset is a two dimensional array of 8x5 dimension;
  size_t d1 = 2048; 
  size_t d2 = 2048; 
  int niter = 10; 
  char scratch[255] = "./";
  double sleep=0.0;
  bool collective=false;
  for(int i=1; i<argc; i++) {
    if (strcmp(argv[i], "--dim")==0) {
      d1 = int(atoi(argv[i+1])); 
      d2 = int(atoi(argv[i+2])); 
      i+=2; 
    } else if (strcmp(argv[i], "--niter")==0) {
      niter = int(atoi(argv[i+1])); 
      i+=1; 
    } else if (strcmp(argv[i], "--scratch")==0) {
      strcpy(scratch, argv[i+1]);
      i+=1;
    } else if (strcmp(argv[i],"--sleep")==0) {
      sleep = atof(argv[i+1]); 
      i+=1; 
    } else if (strcmp(argv[i], "--collective")==0) {
      collective = true;
    }
  }
  hsize_t ldims[2] = {d1, d2};

  hsize_t oned = d1*d2;
  MPI_Comm comm = MPI_COMM_WORLD;
  MPI_Info info = MPI_INFO_NULL;
  int rank, nproc, provided; 
  MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
  MPI_Comm_size(comm, &nproc);
  MPI_Comm_rank(comm, &rank);
  if (rank==0) cout << "MPI_Init_thread provided: " << provided << endl;
  Timing tt(rank==io_node());
  tt.start_clock("total");
  //printf("     MPI: I am rank %d of %d \n", rank, nproc);
  // find local array dimension and offset; 
  hsize_t gdims[2] = {d1*nproc, d2};
  if (rank==0) {
    printf("=============================================\n");
    printf(" Buf dim: %llu x %llu\n",  ldims[0], ldims[1]);
    printf("Buf size: %f MB\n", float(d1*d2)/1024/1024*sizeof(int));
    printf(" Scratch: %s\n", scratch); 
    printf("   nproc: %d\n", nproc);
    printf("=============================================\n");
    if (cache) printf("** using SSD as a cache **\n"); 
  }
  hsize_t offset[2] = {0, 0};
  // setup file access property list for mpio
  hid_t plist_id = H5Pcreate(H5P_FILE_ACCESS);
  H5Pset_fapl_mpio(plist_id, comm, info);

  char f[255];
  strcpy(f, scratch);
  strcat(f, "./parallel_file.h5");
  tt.start_clock("H5Fcreate");   
  hid_t file_id = H5Fcreate(f, H5F_ACC_TRUNC, H5P_DEFAULT, plist_id);
  tt.stop_clock("H5Fcreate"); 
  // create memory space
  hid_t memspace = H5Screate_simple(2, ldims, NULL);

  // define local data
  int* data = new int[ldims[0]*ldims[1]*2];
  // set up dataset access property list 
  hid_t dxf_id = H5Pcreate(H5P_DATASET_XFER);
  if (collective)
    H5Pset_dxpl_mpio(dxf_id, H5FD_MPIO_COLLECTIVE);
  else
    H5Pset_dxpl_mpio(dxf_id, H5FD_MPIO_INDEPENDENT);
  
  hsize_t ggdims[2] = {gdims[0]*niter, gdims[1]};
  hid_t filespace = H5Screate_simple(2, ggdims, NULL);

  hid_t dt = H5Tcopy(H5T_NATIVE_INT);
  tt.start_clock("H5Dcreate");
  hid_t dset_id = H5Dcreate(file_id, "dset", dt, filespace, H5P_DEFAULT,
			    H5P_DEFAULT, H5P_DEFAULT);
  tt.stop_clock("H5Dcreate"); 
  hsize_t size = get_buf_size(memspace, dt);
  if (rank==0) 
    printf(" Total mspace size: %5.5f MB | sizeof (element) %lu\n", float(size)/1024/1024, H5Tget_size(H5T_NATIVE_INT));
  size = get_buf_size(filespace, dt);
  if (rank==0) 
    printf(" Total fspace size: %5.5f MB \n", float(size)/1024/1024);
  
  hsize_t count[2] = {1, 1};


  for (int i=0; i<niter; i++) {
    tt.start_clock("Init_array");     
    for(int j=0; j<ldims[0]*ldims[1]; j++)
      data[j] = i;
    for(int j=ldims[0]*ldims[1]; j<2*ldims[0]*ldims[1]; j++)
      data[j] = i+niter;
    tt.stop_clock("Init_array");
    hsize_t loc_buf_dim[2] = {d1, d2};
    hsize_t data_dim[2] = {2*d1, d2};
    // select hyperslab
    tt.start_clock("Select");
    hid_t filespace = H5Screate_simple(2, ggdims, NULL);
    hsize_t loc_buf_select[2] = {d1/2, d2};
    hid_t memspace = H5Screate_simple(2, data_dim, NULL);
    offset[0]= i*gdims[0] + rank*ldims[0];
    H5Sselect_hyperslab(filespace, H5S_SELECT_SET, offset, NULL, loc_buf_dim, count);
    offset[0]=0;
    H5Sselect_hyperslab(memspace, H5S_SELECT_SET, offset, NULL, loc_buf_select, count);
    offset[0]=d1;
    H5Sselect_hyperslab(memspace, H5S_SELECT_OR, offset, NULL, loc_buf_select, count);
    tt.stop_clock("Select");
    if (rank==0 and i==0) printf(" Selected buffer size (Bytes): %llu (memspace) - %llu (filespace) \n", get_buf_size(memspace, H5T_NATIVE_INT), get_buf_size(filespace, H5T_NATIVE_INT));

    MPI_Barrier(MPI_COMM_WORLD);
    tt.start_clock("H5Dwrite");
    hid_t status = H5Dwrite(dset_id, H5T_NATIVE_INT, memspace, filespace, dxf_id, data); // write memory to file
    tt.stop_clock("H5Dwrite");
    if (rank==0) printf("  *Iter: %d -   write rate: %f MiB/s\n", i, get_buf_size(memspace, H5T_NATIVE_INT)/tt["H5Dwrite"].t_iter[i]*nproc/1024/1024);
    tt.start_clock("compute");
    msleep(int(sleep*1000));
    tt.stop_clock("compute");
  }
  tt.start_clock("H5Dclose");
  H5Dclose(dset_id);
  tt.stop_clock("H5Dclose");
  tt.start_clock("H5Fclose");
  H5Fclose(file_id);
  tt.stop_clock("H5Fclose");
  H5Pclose(dxf_id);
  H5Pclose(plist_id);
  H5Sclose(memspace);
  bool master = (rank==0); 
  delete [] data;
  Timer T = tt["H5Dwrite"]; 
  double avg = 0.0; 
  double std = 0.0; 
  stat(&T.t_iter[0], niter, avg, std, 'i');
  if (rank==0) printf("Overall write rate: %f +/- %f MB/s\n", size*avg/niter/1024/1024, size/niter*std/1024/1024);
  tt.stop_clock("total");
  MPI_Finalize();
  return 0;
}