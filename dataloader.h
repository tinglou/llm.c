/*
Implements a medium simple DataLoader for a distributed training setup.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
// defines: fopenCheck, freadCheck, fcloseCheck, fseekCheck
// defines: mallocCheck
#include "utils.h"

// ----------------------------------------------------------------------------
// we need glob to list files matching a pattern
// windows does not have glob, so we fall back on a very simple implementation
// this implementation doesn't actually do a glob, it assumes that the "pattern"
// is exactly the single file of interest
#ifndef _WIN32
#include <glob.h>
#else

typedef struct glob_t {
    size_t gl_pathc;
    char **gl_pathv;
} glob_t;

int glob(const char *pattern, int flags, void *unused, glob_t *pglob) {
    assert(strstr(pattern, "*") == NULL); // we don't support * here
    pglob->gl_pathc = 1;
    pglob->gl_pathv = (char **)malloc(sizeof(char *));
    if (pglob->gl_pathv == NULL) { exit(EXIT_FAILURE); } // ??? oom?
    pglob->gl_pathv[0] = (char *)pattern;
    return 0;
}

void globfree(glob_t* pglob) {
    free(pglob->gl_pathv);
}
#endif

// ----------------------------------------------------------------------------
// Distributed Data Loader
#define HEADER_SIZE 256

typedef struct {
    // variables related to distributed training
    // each process/worker has to access different parts of the data
    int process_rank;
    int num_processes;
    // hyperparameters. use size_t to prevent overflow
    size_t B;
    size_t T;
    // input handling and its state
    glob_t glob_result; // stores the result of glob, for all shards we want to iterate
    int current_shard; // the current shard we are reading from
    FILE* tokens_file;
    long file_size;
    long current_position;
    uint16_t* buffer; // we fread data from file into this buffer
    // public variables that could be accessed from outside
    size_t num_batches;
    int* inputs;  // input tokens into transformer
    int* targets; // target tokens for the transformer
} DataLoader;

long dataloader_load_shard_(DataLoader *loader, int shard_index) {
    // use the first glob match as the filename for now
    const char* filename = loader->glob_result.gl_pathv[shard_index];
    // open the input file for reading. also only a single file can be opened at a time
    if (loader->tokens_file != NULL) {
        fcloseCheck(loader->tokens_file);
    }
    loader->tokens_file = fopenCheck(filename, "rb");
    // validate the header
    int header[HEADER_SIZE];
    freadCheck(header, sizeof(int), HEADER_SIZE, loader->tokens_file);
    if (header[0] != 20240520) {
        printf("Bad magic in the data file\n");
        printf("---> HINT: Are you passing in a correct file?\n");
        printf("---> HINT: The data encoding may have changed, re-run data prepro or refer again to README.\n");
        exit(EXIT_FAILURE);
    }
    if (header[1] != 1) { printf("Bad version in data file\n"); exit(EXIT_FAILURE); }
    long ntok = header[2]; // number of tokens in the file
    assert(ntok > 0); // we expect some tokens in the file. this should never trip, right?
    // determine the file size and make sure it is consistent with the number of tokens
    fseekCheck(loader->tokens_file, 0, SEEK_END); // seek to end of file
    loader->file_size = ftell(loader->tokens_file); // read the offset, i.e. file size
    fseekCheck(loader->tokens_file, 0, SEEK_SET); // seek back to the beginning
    // we expect ntok in the file to be consistent with filesize, assert that is the case
    long expected_file_size = HEADER_SIZE * sizeof(int) + ntok * sizeof(uint16_t);
    if (loader->file_size != expected_file_size) {
        printf("Error: file size is not as expected\n");
        exit(EXIT_FAILURE);
    }
    return ntok;
}

void dataloader_reset(DataLoader *loader) {
    // fully resets the DataLoader object to init configuration
    // each process starts at a different offset in the file
    long header_bytes = HEADER_SIZE * sizeof(int);
    long token_bytes_offset = loader->process_rank * loader->B * loader->T * sizeof(uint16_t);
    loader->current_shard = 0;
    loader->current_position = header_bytes + token_bytes_offset;
    dataloader_load_shard_(loader, loader->current_shard);
}

void dataloader_advance_(DataLoader *loader) {
    // advance the loader by loading the next data shard and resetting the position
    if (loader->glob_result.gl_pathc > 1) {
        // if we have more than one shard, advance to the next one
        loader->current_shard = (loader->current_shard + 1) % loader->glob_result.gl_pathc;
        dataloader_load_shard_(loader, loader->current_shard);
    }
    long header_bytes = HEADER_SIZE * sizeof(int);
    long token_bytes_offset = loader->process_rank * loader->B * loader->T * sizeof(uint16_t);
    loader->current_position = header_bytes + token_bytes_offset;
}

void dataloader_init(DataLoader *loader,
                     const char* filename_pattern,
                     size_t B,
                     size_t T,
                     int process_rank,
                     int num_processes) {
    loader->process_rank = process_rank;
    loader->num_processes = num_processes;
    loader->B = B;
    loader->T = T;
    loader->tokens_file = NULL;

    // glob to get the list of files matching the pattern, these are our data shards
    int glob_status = glob(filename_pattern, 0, NULL, &loader->glob_result);
    if (glob_status != 0) {
        printf("Error: failed to glob pattern: %s\n", filename_pattern);
        exit(EXIT_FAILURE);
    }
    if (loader->glob_result.gl_pathc == 0) {
        printf("Error: no files found matching the pattern: %s\n", filename_pattern);
        exit(EXIT_FAILURE);
    }

    // inspect and validate all shards so we don't get any runtime errors later
    // if too slow / too many shards, may wish to revisit later
    long ntok_total = 0;
    for (int shard_index = 0; shard_index < loader->glob_result.gl_pathc; shard_index++) {
        long shard_ntok = dataloader_load_shard_(loader, shard_index);
        // we need at least one batch/shard, the way things are written right now.
        // can be relaxed a lot later.
        assert(shard_ntok >= num_processes * B * T + 1);
        ntok_total += shard_ntok;
    }
    // debugging prints
    // printf("DataLoader: filename_pattern: %s\n", filename_pattern);
    // printf("DataLoader: Found %ld tokens across %zu shards\n", ntok_total, loader->glob_result.gl_pathc);

    // allocate all the space we'll need
    loader->buffer = (uint16_t*)malloc((B * T + 1) * sizeof(uint16_t));
    loader->inputs = (int*)malloc(B * T * sizeof(int));
    loader->targets = (int*)malloc(B * T * sizeof(int));
    loader->num_batches = ntok_total / (num_processes * B * T); // useful to know

    // reset the loader, to initialize it
    dataloader_reset(loader);
}

void dataloader_next_batch(DataLoader *loader) {
    size_t B = loader->B;
    size_t T = loader->T;
    // read B*T+1 uint16_t tokens from the file into buffer
    fseekCheck(loader->tokens_file, loader->current_position, SEEK_SET);
    freadCheck(loader->buffer, sizeof(uint16_t), B*T+1, loader->tokens_file);
    // decode the buffer into inputs and targets (cast to int)
    for (int i = 0; i < B*T; i++) {
        loader->inputs[i] = (int)loader->buffer[i];
        loader->targets[i] = (int)loader->buffer[i+1];
    }
    // advance the current position by B*T*num_processes integers
    // note: the "stride" of tokens by which we move each time is definitely B * T
    // we only load B * T + 1 tokens at each iteration because the targets are offset by 1
    loader->current_position += loader->num_processes * B * T * sizeof(uint16_t);
    // if the next batch would go past the end of the file, advance the loader
    if (loader->current_position + (loader->num_processes * B * T + 1) * sizeof(uint16_t) > loader->file_size) {
        dataloader_advance_(loader);
    }
}

void dataloader_free(DataLoader *loader) {
    free(loader->buffer);
    free(loader->inputs);
    free(loader->targets);
    fcloseCheck(loader->tokens_file);
    globfree(&loader->glob_result);
}

// ----------------------------------------------------------------------------
// Distributed Eval Loader
// Many evals (like) HellaSwag and MMLU are multiple-choice
// where there are 4 possible continuations and a label for the correct one
// We want to load and serve these style of evals
/*
Copy pasting the section on the eval datafile format, from data_common.py:
- First comes a header with 256 int32s
- The examples follow, each example is a stream of uint16_t:
    - <START_EXAMPLE> delimiter of 2**16-1, i.e. 65,535
    - <EXAMPLE_BYTES>, bytes encoding this example, allowing efficient skip to next
    - <EXAMPLE_INDEX>, the index of the example in the dataset
    - <LABEL>, the index of the correct completion
    - <NUM_COMPLETIONS>, indicating the number of completions (usually 4)
    - <NUM><CONTEXT_TOKENS>, where <NUM> is the number of tokens in the context
    - <NUM><COMPLETION_TOKENS>, repeated NUM_COMPLETIONS times
*/

// for now, could relax later
#define ASSUMED_NUM_COMPLETIONS 4
// helper macro for ceildiv
#define CEIL_DIV(M, N) (((M) + (N)-1) / (N))

typedef struct {
    // variables related to distributed training
    // each process/worker has to access different parts of the data
    int process_rank;
    int num_processes;
    // hyperparameters. use size_t to prevent overflow
    size_t B; // (micro) batch size dimension of the tensor that feeds into the model
    size_t T; // maximum context length of the model
    // input handling and its state
    FILE* eval_file;
    long file_size;
    uint16_t* buffer; // we fread data from file into this buffer
    // public variables that could be accessed from outside
    int num_examples; // in total across all processes
    int num_batches; // to process the entire dataset across all processes
    int start_example_index; // the assignment of work for this process, start
    int end_example_index; // and end. start is inclusive, end is exclusive
    int current_example_index; // the next example we would read
    int* inputs;  // input tokens into transformer
    int* targets; // target tokens for the transformer
    char* mask; // mask=1 at all completion token locations
    int* label; // the correct completion labels
    int num_completions; // number of completions for this example
} EvalLoader;

void evalloader_reset(EvalLoader *loader) {
    // we have to be careful that each process starts at the correct offset.
    // For example if there are N examples in the file and 4 processes,
    // then process 0 should start at 0, process 1 at N/4, process 2 at N/2, etc.
    // determine how much work there is for all processes
    int examples_per_process = CEIL_DIV(loader->num_examples, loader->num_processes);
    int can_fit_examples = loader->B / ASSUMED_NUM_COMPLETIONS;
    loader->num_batches = CEIL_DIV(examples_per_process, can_fit_examples);
    // determine the start and end example indices for this process
    loader->start_example_index = examples_per_process * loader->process_rank;
    loader->end_example_index = examples_per_process * (loader->process_rank + 1);
    // crop the end example index to the total number of examples
    if (loader->end_example_index > loader->num_examples) {
        loader->end_example_index = loader->num_examples;
    }
    // now seek through the file to the start of that example
    // utilize <EXAMPLE_BYTES> for efficiency
    long header_bytes = HEADER_SIZE * sizeof(int);
    fseekCheck(loader->eval_file, header_bytes, SEEK_SET);
    for (int i = 0; i < loader->start_example_index; i++) {
        uint16_t example_header[3];
        // read 3 uint16_t values: <START_EXAMPLE>, <EXAMPLE_BYTES>, <EXAMPLE_INDEX>
        freadCheck(&example_header[0], sizeof(uint16_t), 3, loader->eval_file);
        // validate the <START_EXAMPLE> delimiter
        assert(example_header[0] == 65535); // <START_EXAMPLE> delimiter
        // validate the <EXAMPLE_INDEX>
        assert(example_header[2] == i); // <EXAMPLE_INDEX> should match the loop index
        // skip to the next example, keeping in mind that we already read the header
        size_t remaining_bytes = example_header[1] - sizeof(uint16_t) * 3;
        assert(remaining_bytes > 0); // we expect some bytes in the example
        fseekCheck(loader->eval_file, remaining_bytes, SEEK_CUR);
    }
    // now we are at the start of the example we want to start at, pointing at <START_EXAMPLE>
    loader->current_example_index = loader->start_example_index;
}

void evalloader_init(EvalLoader *loader,
                     const char* filename,
                     size_t B,
                     size_t T,
                     int process_rank,
                     int num_processes) {
    loader->process_rank = process_rank;
    loader->num_processes = num_processes;
    loader->B = B;
    loader->T = T;

    // open the file and validate the header
    loader->eval_file = fopenCheck(filename, "rb");
    // validate the header
    int header[HEADER_SIZE];
    freadCheck(header, sizeof(int), HEADER_SIZE, loader->eval_file);
    if (header[0] != 20240522) { printf("Bad magic in eval file\n"); exit(EXIT_FAILURE); }
    if (header[1] != 1) { printf("Bad version in data file\n"); exit(EXIT_FAILURE); }
    loader->num_examples = header[2]; // number of tokens in the file
    assert(loader->num_examples >= num_processes); // avoid headaches for now
    size_t longest_example_bytes = header[3]; // longest example in the file
    // basic sensibility check we could relax later. but roughly each example
    // contains the prompt (or "context") and 4 completions, all of these have to be
    // up to T tokens, and their tokens are uint16_t (so 2 bytes/token).
    // There's a few more things in each example but they are minor.
    // So longest example should be roughly this. Just trying to make sure it's sensible.
    assert(longest_example_bytes > 0 && longest_example_bytes < (1+ASSUMED_NUM_COMPLETIONS)*T*2);

    // allocate all the space we'll need
    int can_fit_examples = B / ASSUMED_NUM_COMPLETIONS;
    loader->buffer = (uint16_t*)malloc(longest_example_bytes);
    loader->inputs = (int*)malloc(B * T * sizeof(int));
    loader->targets = (int*)malloc(B * T * sizeof(int));
    loader->mask = (char*)malloc(B * T * sizeof(char));
    loader->label = (int*)malloc(can_fit_examples * sizeof(int));

    // reset the loader, to initialize it
    evalloader_reset(loader);
}

void evalloader_next_example_(EvalLoader *loader, int example_batch_index) {
    // this function populates the inputs, targets, mask, and label fields for one example
    // because every (B,T) tensor can fit multiple examples and we want to take advantage,
    // we also pass in the example_batch_index to indicate which example in the batch we are loading
    // and each example takes up ASSUMED_NUM_COMPLETIONS rows in the batch
    size_t B = loader->B;
    size_t T = loader->T;
    int batch_dim_offset = example_batch_index * ASSUMED_NUM_COMPLETIONS;
    // read the current example header
    uint16_t example_header[3];
    freadCheck(&example_header[0], sizeof(uint16_t), 3, loader->eval_file);
    // validate the <START_EXAMPLE> delimiter
    assert(example_header[0] == 65535); // <START_EXAMPLE> delimiter
    // validate the <EXAMPLE_INDEX>
    assert(example_header[2] == loader->current_example_index); // <EXAMPLE_INDEX> should match the loop index
    assert(example_header[2] >= loader->start_example_index && example_header[2] < loader->end_example_index);
    // read the rest of the example (we have space for 3 more uint16_t values in buffer, it's ok)
    size_t example_bytes = example_header[1] - sizeof(uint16_t) * 3;
    // read example_bytes into buffer. careful that this is actually in the units of bytes
    freadCheck(loader->buffer, sizeof(char), example_bytes, loader->eval_file);
    // process the example label
    int label = (int)loader->buffer[0];
    int can_fit_examples = loader->B / ASSUMED_NUM_COMPLETIONS;
    assert(label >= 0 && label < ASSUMED_NUM_COMPLETIONS); // we expect the label to be in [0, 4) for right now
    assert(example_batch_index >= 0 && example_batch_index < can_fit_examples);
    loader->label[example_batch_index] = label; // store for output
    // process the number of completions
    int num_completions = (int)loader->buffer[1];
    assert(num_completions == ASSUMED_NUM_COMPLETIONS); // we expect 4 completions for now
    assert(batch_dim_offset + num_completions <= B); // we expect to fit in the batch
    loader->num_completions = num_completions; // store for output
    // process the context
    // the context is shared for all completions, so we insert it into all data rows equally
    int context_length = (int)loader->buffer[2];
    uint16_t *context_tokens_start = &loader->buffer[3]; // where the tokens start
    assert(context_length > 0 && context_length < T); // context is non-empty and up to T
    for (int b = 0; b < num_completions; b++) {
        for (int i = 0; i < context_length; i++) {
            int boff = batch_dim_offset + b;
            int tok_cur = (int)context_tokens_start[i];
            loader->inputs[boff * T + i] = tok_cur;
        }
    }
    // process the completions, insert them in their row, right after the (shared) context
    uint16_t *completions_iter = loader->buffer + 3 + context_length;
    for (int c = 0; c < num_completions; c++) {
        int coff = batch_dim_offset + c;
        int completion_length = (int)completions_iter[0];
        uint16_t *completion_tokens_start = completions_iter + 1;
        assert(completion_length > 0 && context_length + completion_length < T); // things fit?
        for (int i = 0; i < completion_length; i++) {
            int tok_cur = (int)completion_tokens_start[i];
            // at inputs, the completions simply follow the context
            loader->inputs[coff * T + context_length + i] = tok_cur;
            // at targets things start to get tricky
            // we expect the last context token to predict the first completion token
            // and then onwards from there.
            loader->targets[coff * T + context_length + i - 1] = tok_cur;
            // and at these positions, we want to set mask=1, because these are the
            // positions where we want to average the loss, in each row, to determine
            // its overall probability of following the context.
            loader->mask[coff * T + context_length + i - 1] = 1;
        }
        completions_iter += 1 + completion_length; // move to the next completion
    }
    // advance the current example to point to the next one we'd load
    loader->current_example_index += 1;
}

void evalloader_next_batch(EvalLoader *loader) {
    size_t B = loader->B;
    size_t T = loader->T;
    // init all inputs, targets, mask to zeros
    // TODO: I think only mask is necessary to reset?
    memset(loader->inputs, 0, B * T * sizeof(int));
    memset(loader->targets, 0, B * T * sizeof(int));
    memset(loader->mask, 0, B * T * sizeof(char));
    // ok here is the problem we are solving
    // we have a batch dimension of B, which we want to take full advantage of
    // each example has some number of completions (usually 4)
    // so we want to pack as many examples into rows of B as we can fit
    int can_fit_examples = B / ASSUMED_NUM_COMPLETIONS; // how many examples can we fit in the batch?
    for (int i = 0; i < can_fit_examples; i++) {
        if (loader->current_example_index >= loader->end_example_index) {
            break; // this process has exhausted its work, noop from here on
        }
        evalloader_next_example_(loader, i);
    }
}

int evalloader_stat_losses(EvalLoader *loader, float* losses) {
    // compute statistics of losses (B*T) resulting from a forward pass
    // on a batch that was constructed from EvalLoader
    // putting this functionality here because it is tightly coupled
    // with how we construct and represent the data batches.
    // returns the number of correct examples in this batch.
    int correct = 0;
    size_t B = loader->B;
    size_t T = loader->T;
    // iterate the examples in this batch
    int can_fit_examples = B / ASSUMED_NUM_COMPLETIONS;
    for (int i = 0; i < can_fit_examples; i++) {
        float min_loss;
        int min_loss_index;
        char active = 0; // is this example active or fully empty?
        // iterate the completions in this example
        for (int b = 0; b < ASSUMED_NUM_COMPLETIONS; b++) {
            int boff = i * ASSUMED_NUM_COMPLETIONS + b;
            // evaluate the quality of this completion
            // its quality is simply the average loss over the tokens
            float average_loss = 0.0f;
            int count = 0;
            for (int t = 0; t < T; t++) {
                char mask = loader->mask[boff * T + t];
                if (mask == 1) {
                    active = 1;
                    average_loss += losses[boff * T + t];
                    count++;
                }
            }
            if (count > 0) { average_loss /= count; }
            if (b == 0 || average_loss < min_loss) {
                min_loss = average_loss;
                min_loss_index = b;
            }
        }
        if (active && (min_loss_index == loader->label[i])) {
            correct += 1;
        }
    }
    return correct;
}

void evalloader_free(EvalLoader *loader) {
    free(loader->buffer);
    free(loader->inputs);
    free(loader->targets);
    free(loader->mask);
    free(loader->label);
    fcloseCheck(loader->eval_file);
}