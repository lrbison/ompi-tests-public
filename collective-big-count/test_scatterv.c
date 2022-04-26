/*
 * Copyright (c) 2021-2022 IBM Corporation.  All rights reserved.
 *
 * $COPYRIGHT$
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <mpi.h>
#include "common.h"

int my_c_test_core(MPI_Datatype dtype, size_t total_num_elements, int mode, bool blocking);

int main(int argc, char** argv) {
    /*
     * Initialize the MPI environment
     */
    int ret = 0;

    MPI_Init(NULL, NULL);
    init_environment(argc, argv);

    // Run the tests
#ifndef TEST_UNIFORM_COUNT
    // Each rank contribues: V_SIZE_INT / world_size elements
    // Largest buffer is   : V_SIZE_INT elements
    ret += my_c_test_core(MPI_INT, V_SIZE_INT,MODE_PACKED, true);
    // Adjust these to be V_SIZE_INT - displacement strides so it will pass
    ret += my_c_test_core(MPI_INT,
                          (V_SIZE_INT - disp_stride*world_size),
                          MODE_SKIP, true);

    ret += my_c_test_core(MPI_C_DOUBLE_COMPLEX, V_SIZE_DOUBLE_COMPLEX, MODE_PACKED,
                          true);
    // Adjust these to be V_SIZE_INT - displacement strides so it will pass
    ret += my_c_test_core(MPI_C_DOUBLE_COMPLEX,
                          (V_SIZE_DOUBLE_COMPLEX - disp_stride*world_size),
                          MODE_SKIP, true);
    if (allow_nonblocked) {
        ret += my_c_test_core(MPI_INT, V_SIZE_INT,MODE_PACKED, false);
        // Adjust these to be V_SIZE_INT - displacement strides so it will pass
        ret += my_c_test_core(MPI_INT,
                              (V_SIZE_INT - disp_stride*world_size),
                              MODE_SKIP, false);
        ret += my_c_test_core(MPI_C_DOUBLE_COMPLEX, V_SIZE_DOUBLE_COMPLEX, MODE_PACKED,
                              false);
        // Adjust these to be V_SIZE_INT - displacement strides so it will pass
        ret += my_c_test_core(MPI_C_DOUBLE_COMPLEX,
                              (V_SIZE_DOUBLE_COMPLEX - disp_stride*world_size),
                              MODE_SKIP, false);
    }
#else
    size_t proposed_count;

    // Each rank contribues: TEST_UNIFORM_COUNT elements
    // Largest buffer is   : TEST_UNIFORM_COUNT x world_size

    // Note: Displacement is an int, so the recv buffer cannot be too large as to overflow the int
    // As such divide by the world_size
    proposed_count = calc_uniform_count(sizeof(int), TEST_UNIFORM_COUNT / (size_t)world_size,
                                        (size_t)world_size, 1);
    ret += my_c_test_core(MPI_INT, proposed_count * (size_t)world_size, MODE_PACKED, true);
    // Adjust these to be V_SIZE_INT - displacement strides so it will pass
    ret += my_c_test_core(MPI_INT,
                          (proposed_count - disp_stride*world_size) * (size_t)world_size,
                          MODE_SKIP, true);

    // Note: Displacement is an int, so the recv buffer cannot be too large as to overflow the int
    // As such divide by the world_size
    proposed_count = calc_uniform_count(sizeof(double _Complex), TEST_UNIFORM_COUNT / (size_t)world_size,
                                        (size_t)world_size, 1);
    ret += my_c_test_core(MPI_C_DOUBLE_COMPLEX, proposed_count * (size_t)world_size, MODE_PACKED, true);
    // Adjust these to be V_SIZE_INT - displacement strides so it will pass
    ret += my_c_test_core(MPI_C_DOUBLE_COMPLEX,
                          (proposed_count - disp_stride*world_size) * (size_t)world_size,
                          MODE_SKIP, true);
    if (allow_nonblocked) {
        proposed_count = calc_uniform_count(sizeof(int), TEST_UNIFORM_COUNT / (size_t)world_size,
                                            (size_t)world_size, 1);
        ret += my_c_test_core(MPI_INT, proposed_count * (size_t)world_size, MODE_PACKED, false);
        ret += my_c_test_core(MPI_INT,
                              (proposed_count - disp_stride*world_size) * (size_t)world_size,
                              MODE_SKIP, false);
        proposed_count = calc_uniform_count(sizeof(double _Complex), TEST_UNIFORM_COUNT / (size_t)world_size,
                                            (size_t)world_size, 1);
        ret += my_c_test_core(MPI_C_DOUBLE_COMPLEX, proposed_count * (size_t)world_size, MODE_PACKED,
                              false);
        ret += my_c_test_core(MPI_C_DOUBLE_COMPLEX,
                              (proposed_count - disp_stride*world_size) * (size_t)world_size,
                              MODE_SKIP, false);
    }
#endif

    /*
     * All done
     */
    MPI_Finalize();
    return ret;
}

int my_c_test_core(MPI_Datatype dtype, size_t total_num_elements, int mode, bool blocking)
{
    int ret = 0;
    size_t i;
    MPI_Request request;
    char *mpi_function = blocking ? "MPI_Scatterv" : "MPI_Iscatterv";

    // Actual payload size as divisible by the sizeof(dt)
    size_t payload_size_actual;

    /*
     * Initialize vector
     */
    int *my_int_send_vector = NULL;
    int *my_int_recv_vector = NULL;
    int int_exp;

    double _Complex *my_dc_send_vector = NULL;
    double _Complex *my_dc_recv_vector = NULL;
    double _Complex dc_exp;

    int *my_send_counts = NULL;
    int *my_send_disp = NULL;
    int recv_count = 0;
    int d_idx, r_idx;
    size_t last_disp, last_count;
    size_t num_wrong = 0;
    size_t v_size, v_rem;

    assert(MPI_INT == dtype || MPI_C_DOUBLE_COMPLEX == dtype);

    // total_num_elements = send_size (at root)
    // recv_count         = recv_count
    v_size = total_num_elements / world_size;
    v_rem  = total_num_elements % world_size;
    if (0 != v_rem && world_rank == world_size-1) {
        v_size += v_rem;
    }
    assert(recv_count <= INT_MAX);
    recv_count = (int)v_size;

    if (world_rank == 0) {
        if( MODE_PACKED == mode ) {
            /* Strategy for testing:
             *  - Displacement should skip 0 elements producing a tightly packed buffer
             *  - Count will be the same at all ranks
             *  - buffer can be v_size elements in size
             *
             * NP = 4 and total_num_elements = 9 then the final buffer will be:
             * [1, 1, 2, 2, 3, 3, 4, 4, 4]
             */
            if( MPI_INT == dtype ) {
                payload_size_actual = total_num_elements * sizeof(int);
                my_int_send_vector = (int*)safe_malloc(payload_size_actual);
            } else {
                payload_size_actual = total_num_elements * sizeof(double _Complex);
                my_dc_send_vector = (double _Complex*)safe_malloc(payload_size_actual);
            }
            my_send_counts = (int*)safe_malloc(sizeof(int) * world_size);
            my_send_disp   = (int*)safe_malloc(sizeof(int) * world_size);
            last_disp = 0;
            last_count = v_size;

            for(d_idx = 0; d_idx < world_size; ++d_idx) {
                if (0 != v_rem && d_idx == world_size-1) {
                    last_count += v_rem;
                }
                assert(last_count <= INT_MAX);
                my_send_counts[d_idx] = (int)last_count;
                assert(last_disp <= INT_MAX);
                my_send_disp[d_idx]   = (int)last_disp;
                if( debug > 0 ) {
                    printf("d_idx %3d / last_disp %9d / last_count %9d | total_count %10zu / payload_size %10zu\n",
                           d_idx, (int)last_disp, (int)last_count, total_num_elements, payload_size_actual);
                }
                // Shift displacement by the count for tightly packed buffer
                last_disp += last_count;
            }

            r_idx = 0;
            if( world_size > 1 ) {
                last_disp = my_send_counts[r_idx] + my_send_disp[r_idx];
            } else {
                last_disp = 0;
            }
            if( MPI_INT == dtype ) {
                for(i = 0; i < total_num_elements; ++i) {
                    if( world_size > r_idx+1 && i == last_disp ) {
                        ++r_idx;
                        last_disp = my_send_counts[r_idx] + my_send_disp[r_idx];
                    }
                    my_int_send_vector[i] = 1 + r_idx;
                }
            } else {
                for(i = 0; i < total_num_elements; ++i) {
                    if( world_size > r_idx+1 && i == last_disp ) {
                        ++r_idx;
                        last_disp = my_send_counts[r_idx] + my_send_disp[r_idx];
                    }
                    my_dc_send_vector[i] = 1.0*(1+r_idx) + 1.0*(1+r_idx)*I;
                }
            }
        } else {
            /* Strategy for testing:
             *  - Displacement should skip 2 elements before first element and between each peer making a small gap
             *  - Count will be the same at all ranks +/- and divisible by v_size
             *  - buffer can be v_size + gaps for displacements
             *
             * NP = 4 and total_num_elements = 9 (17 with stride) then the final buffer will be:
             * [-1, -1, 1, 1, -1, -1, 2, 2, -1, -1, 3, 3, -1, -1, 4, 4, 4]
             */
            total_num_elements += disp_stride * (size_t)world_size;

            if( MPI_INT == dtype ) {
                payload_size_actual = total_num_elements * sizeof(int);
                my_int_send_vector = (int*)safe_malloc(payload_size_actual);
            } else {
                payload_size_actual = total_num_elements * sizeof(double _Complex);
                my_dc_send_vector = (double _Complex*)safe_malloc(payload_size_actual);
            }
            my_send_counts = (int*)safe_malloc(sizeof(int) * world_size);
            my_send_disp   = (int*)safe_malloc(sizeof(int) * world_size);
            last_disp = disp_stride;
            last_count = v_size;

            for(d_idx = 0; d_idx < world_size; ++d_idx) {
                if (0 != v_rem && d_idx == world_size-1) {
                    last_count += v_rem;
                }
                assert(last_count <= INT_MAX);
                my_send_counts[d_idx] = (int)last_count;
                assert(last_disp <= INT_MAX);
                my_send_disp[d_idx]   = (int)last_disp;
                if( debug  > 0) {
                    printf("d_idx %3d / last_disp %9d / last_count %9d | total_count %10zu / payload_size %10zu\n",
                           d_idx, (int)last_disp, (int)last_count, total_num_elements, payload_size_actual);
                }
                // Shift displacement by the count for tightly packed buffer
                last_disp += last_count + disp_stride;
            }

            r_idx = 0;
            if( world_size > 1 ) {
                last_disp = my_send_counts[r_idx] + my_send_disp[r_idx];
            } else {
                last_disp = 0;
            }

            if( MPI_INT == dtype ) {
                for(i = 0; i < total_num_elements; ++i) {
                    if( world_size > r_idx+1 && i == last_disp ) {
                        ++r_idx;
                        last_disp = my_send_counts[r_idx] + my_send_disp[r_idx];
                    }
                    if( i < my_send_disp[r_idx] ) {
                        my_int_send_vector[i] = -1;
                    } else {
                        my_int_send_vector[i] = 1 + r_idx;
                    }
                }
            } else {
                for(i = 0; i < total_num_elements; ++i) {
                    if( world_size > r_idx+1 && i == last_disp ) {
                        ++r_idx;
                        last_disp = my_send_counts[r_idx] + my_send_disp[r_idx];
                    }
                    if( i < my_send_disp[r_idx] ) {
                        my_dc_send_vector[i] = -1.0 - 1.0*I;
                    } else {
                        my_dc_send_vector[i] = 1.0*(1+r_idx) + 1.0*(1+r_idx)*I;
                    }
                }
            }
        }
    }

    if( MPI_INT == dtype ) {
        my_int_recv_vector = (int*)safe_malloc(sizeof(int) * recv_count);
        for(i = 0; i < recv_count; ++i) {
            my_int_recv_vector[i] = -1;
        }
    } else {
        my_dc_recv_vector = (double _Complex*)safe_malloc(sizeof(double _Complex) * recv_count);
        for(i = 0; i < recv_count; ++i) {
            my_dc_recv_vector[i] = -1.0 - 1.0*I;
        }
    }

    if (world_rank == 0) {
        printf("---------------------\nResults from %s(%s x %zu = %zu or %s): Mode: %s\n",
               mpi_function, (MPI_INT == dtype ? "int" : "double _Complex"),
               total_num_elements, payload_size_actual, human_bytes(payload_size_actual),
               (MODE_PACKED == mode) ? "PACKED" : "SKIPPY");
    }

    if (blocking) {
        if( MPI_INT == dtype ) {
            MPI_Scatterv(my_int_send_vector, my_send_counts, my_send_disp, dtype,
                         my_int_recv_vector, recv_count, dtype,
                         0, MPI_COMM_WORLD);
        } else {
            MPI_Scatterv(my_dc_send_vector, my_send_counts, my_send_disp, dtype,
                         my_dc_recv_vector, recv_count, dtype,
                         0, MPI_COMM_WORLD);
        }
    }
    else {
        if( MPI_INT == dtype ) {
            MPI_Iscatterv(my_int_send_vector, my_send_counts, my_send_disp, dtype,
                         my_int_recv_vector, recv_count, dtype,
                         0, MPI_COMM_WORLD, &request);
        } else {
            MPI_Iscatterv(my_dc_send_vector, my_send_counts, my_send_disp, dtype,
                         my_dc_recv_vector, recv_count, dtype,
                         0, MPI_COMM_WORLD, &request);
        }
        MPI_Wait(&request, MPI_STATUS_IGNORE);
    }

    /*
     * Check results.
     */
    int_exp = 0;

    if( MODE_PACKED == mode ) {
        for(i = 0; i < recv_count; ++i) {
            int_exp = 1 + world_rank;
            if( MPI_INT == dtype ) {
                if( debug > 1) {
                    printf("%2d CHECK: %2zu : %3d vs %3d\n",
                           world_rank, i, my_int_recv_vector[i], int_exp);
                }
                if(my_int_recv_vector[i] != int_exp) {
                    ++num_wrong;
                }
            } else {
                dc_exp = 1.0*int_exp + 1.0*int_exp*I;
                if( debug > 1) {
                    printf("%2d CHECK: %2zu : (%14.0f,%14.0fi) vs (%14.0f,%14.0fi)\n",
                           world_rank, i, creal(my_dc_recv_vector[i]), cimag(my_dc_recv_vector[i]), creal(dc_exp), cimag(dc_exp));
                }
                if(my_dc_recv_vector[i] != dc_exp) {
                    ++num_wrong;
                }
            }
        }
    } else {
        for(i = 0; i < recv_count; ++i) {
            int_exp = 1 + world_rank;
            if( MPI_INT == dtype ) {
                if( debug > 1) {
                    printf("%2d CHECK: %2zu : %3d vs %3d\n",
                           world_rank, i, my_int_recv_vector[i], int_exp);
                }
                if(my_int_recv_vector[i] != int_exp) {
                    ++num_wrong;
                }
            } else {
                dc_exp = 1.0*int_exp + 1.0*int_exp*I;
                if( debug > 1) {
                    printf("%2d CHECK: %2zu : (%14.0f,%14.0fi) vs (%14.0f,%14.0fi)\n",
                           world_rank, i, creal(my_dc_recv_vector[i]), cimag(my_dc_recv_vector[i]), creal(dc_exp), cimag(dc_exp));
                }
                if(my_dc_recv_vector[i] != dc_exp) {
                    ++num_wrong;
                }
            }
        }
    }

    if( 0 == num_wrong) {
        printf("Rank %2d: PASSED\n", world_rank);
    } else {
        printf("Rank %2d: ERROR: DI in %14zu of %14zu slots (%6.1f %% wrong)\n", world_rank,
               num_wrong, total_num_elements, ((num_wrong * 1.0)/total_num_elements)*100.0);
        ret = 1;
    }

    if( NULL != my_int_send_vector ) {
        free(my_int_send_vector);
    }
    if( NULL != my_int_recv_vector ){
        free(my_int_recv_vector);
    }
    if( NULL != my_dc_send_vector ) {
        free(my_dc_send_vector);
    }
    if( NULL != my_dc_recv_vector ){
        free(my_dc_recv_vector);
    }
    fflush(NULL);
    MPI_Barrier(MPI_COMM_WORLD);

    return ret;
}
