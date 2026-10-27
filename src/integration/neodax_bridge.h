#ifndef NEODAX_BRIDGE_H
#define NEODAX_BRIDGE_H

int analyze_with_parallel(const char* input_path, int* output_buffer);
int analyze_with_serial(const char* input_path, int* output_buffer);

#endif
