#!/bin/bash

find ./ -name 'journal.2026*' | xargs rm 
find ./ -name '*.log' | xargs rm
