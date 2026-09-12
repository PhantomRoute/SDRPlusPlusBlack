#pragma once
#include <vector>
#include <map>
#include "processor.h"

namespace dsp {
    template<class T>
    class chain {
    public:
        chain() {}

        chain(stream<T>* in) { init(in); }

        void init(stream<T>* in) {
            _in = in;
            out = _in;
        }

        template<typename Func>
        void setInput(stream<T>* in, Func onOutputChange) {
            _in = in;
            for (auto& ln : links) {
                if (states[ln]) {
                    ln->setInput(_in);
                    return;
                }
            }
            out = _in;
            onOutputChange(out);
        }
        
        void addBlock(Processor<T, T>* block, bool enabled) {
            // Check if block is already part of the chain
            if (blockExists(block)) {
                throw std::runtime_error("[chain] Tried to add a block that is already part of the chain");
            }

            // Add to the list
            links.push_back(block);
            states[block] = false;

            // Enable if needed
            if (enabled) { enableBlock(block, [](stream<T>* out){}); }
        }

        template<typename Func>
        void removeBlock(Processor<T, T>* block, Func onOutputChange) {
            // Check if block is part of the chain
            if (!blockExists(block)) {
                throw std::runtime_error("[chain] Tried to remove a block that is not part of the chain");
            }

            // Disable the block
            disableBlock(block, onOutputChange);
        
            // Remove block from the list
            states.erase(block);
            links.erase(std::find(links.begin(), links.end(), block));
        }

        template<typename Func>
        void enableBlock(Processor<T, T>* block, Func onOutputChange) {
            // Check that the block is part of the chain
            if (!blockExists(block)) {
                throw std::runtime_error("[chain] Tried to enable a block that isn't part of the chain");
            }
            
            // If already enable, don't do anything
            if (states[block]) { return; }

            // Gather blocks before and after the block to enable
            Processor<T, T>* before = blockBefore(block);
            Processor<T, T>* after = blockAfter(block);

            // Update input of next block or output
            if (after) {
                after->setInput(&block->out);
            }
            else {
                out = &block->out;
                onOutputChange(out);
            }

            // Set input of the new block
            block->setInput(before ? &before->out : _in);

            // Start new block
            if (running) { block->start(); }
            states[block] = true;
        }

        template<typename Func>
        void disableBlock(Processor<T, T>* block, Func onOutputChange) {
            // Check that the block is part of the chain
            if (!blockExists(block)) {
                throw std::runtime_error("[chain] Tried to disable a block that isn't part of the chain");
            }
            
            // If already disabled, don't do anything
            if (!states[block]) { return; }

            // Stop disabled block
            block->stop();
            states[block] = false;

            // Gather blocks before and after the block to disable
            Processor<T, T>* before = blockBefore(block);
            Processor<T, T>* after = blockAfter(block);

            // Update input of next block or output
            if (after) {
                after->setInput(before ? &before->out : _in);
            }
            else {
                out = before ? &before->out : _in;
                onOutputChange(out);
            }
        }

        template<typename Func>
        void setBlockEnabled(Processor<T, T>* block, bool enabled, Func onOutputChange) {
            if (enabled) {
                enableBlock(block, onOutputChange);
            }
            else {
                disableBlock(block, onOutputChange);
            }
        }

        template<typename Func>
        void enableAllBlocks(Func onOutputChange) {
            for (auto& ln : links) {
                enableBlock(ln, onOutputChange);
            }
        }

        template<typename Func>
        void disableAllBlocks(Func onOutputChange) {
            for (auto& ln : links) {
                disableBlock(ln, onOutputChange);
            }
        }

        void start() {
            if (running) { return; }
            for (auto& ln : links) {
                if (!states[ln]) { continue; }
                ln->start();
            }
            running = true;
        }

        void stop() {
            if (!running) { return; }
            for (auto& ln : links) {
                if (!states[ln]) { continue; }
                ln->stop();
            }
            running = false;
        }

        stream<T>* out;

    private:
        // The enabled block immediately before this one, which is what a block being
        // enabled must read from.
        //
        // This used to return the FIRST enabled block in the chain instead of the
        // nearest one behind, so in any chain of three or more the block switched on
        // last was wired straight to the front of the chain and everything between was
        // silently cut out of the path - still flagged enabled, still shown in the
        // menu, no longer doing anything. The radio's audio chain is resampler ->
        // de-emphasis -> tone detector, plus whatever blocks other modules insert, so
        // whether de-emphasis was actually in the path came down to the order things
        // happened to be enabled in at startup. That is the "de-emphasis reads 50us
        // after a restart but sounds like None, until you change it by hand" bug: on a
        // fresh start the noise reduction block was enabled last and took its input
        // from the resampler; changing the setting re-enabled de-emphasis, which put
        // it back in front of the noise reduction.
        Processor<T, T>* blockBefore(Processor<T, T>* block) {
            Processor<T, T>* last = NULL;
            for (auto& ln : links) {
                if (ln == block) { return last; }
                if (states[ln]) { last = ln; }
            }
            return NULL;
        }

        Processor<T, T>* blockAfter(Processor<T, T>* block) {
            bool blockFound = false;
            for (auto& ln : links) {
                if (ln == block) {
                    blockFound = true;
                    continue;
                }
                if (states[ln] && blockFound) { return ln; }
            }
            return NULL;
        }

        bool blockExists(Processor<T, T>* block) {
            return states.find(block) != states.end();
        }

        stream<T>* _in;
        std::vector<Processor<T, T>*> links;
        std::map<Processor<T, T>*, bool> states;
        bool running = false;
    };
}