#!/usr/bin/python3
# This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details

import argparse
import json
from collections import Counter
import pandas as pd
## needed for 'to_markdown' method for pandas data frame
import tabulate


def getArgs():
    parser = argparse.ArgumentParser(description='Analyze compiler statistics')
    parser.add_argument('--bytecode-bin-factor', dest='bytecodeBinFactor',default=10,help='Bytecode bin size as a multiple of 1000 (10 by default)')
    parser.add_argument('--block-bin-factor', dest='blockBinFactor',default=1,help='Block bin size as a multiple of 1000 (1 by default)')
    parser.add_argument('--block-instruction-bin-factor', dest='blockInstructionBinFactor',default=1,help='Block bin size as a multiple of 1000 (1 by default)')
    parser.add_argument('statsFile', help='stats.json file generated by running luau-compile')
    args = parser.parse_args()
    return args

def readStats(statsFile):
    with open(statsFile) as f:
        stats = json.load(f)

        scripts = []
        functionCounts = []
        bytecodeLengths = []
        blockPreOptCounts = []
        blockPostOptCounts = []
        maxBlockInstructionCounts = []

        for path, fileStat in stats.items():
            scripts.append(path)
            functionCounts.append(fileStat['lowerStats']['totalFunctions'] - fileStat['lowerStats']['skippedFunctions'])
            bytecodeLengths.append(fileStat['bytecode'])
            blockPreOptCounts.append(fileStat['lowerStats']['blocksPreOpt'])
            blockPostOptCounts.append(fileStat['lowerStats']['blocksPostOpt'])
            maxBlockInstructionCounts.append(fileStat['lowerStats']['maxBlockInstructions'])

        stats_df = pd.DataFrame({
            'Script': scripts,
            'FunctionCount': functionCounts,
            'BytecodeLength': bytecodeLengths,
            'BlockPreOptCount': blockPreOptCounts,
            'BlockPostOptCount': blockPostOptCounts,
            'MaxBlockInstructionCount': maxBlockInstructionCounts
        })

        return stats_df


def analyzeBytecodeStats(stats_df, config):
    binFactor = config.bytecodeBinFactor
    divisor = binFactor * 1000
    totalScriptCount = len(stats_df.index)

    lengthLabels = []
    scriptCounts = []
    scriptPercs = []

    counter = Counter()

    for index, row in stats_df.iterrows():
        value = row['BytecodeLength']
        factor = int(value / divisor)
        counter[factor] += 1

    for factor, scriptCount in sorted(counter.items()):
        left = factor * binFactor
        right = left + binFactor
        lengthLabel = '{left}K-{right}K'.format(left=left, right=right)
        lengthLabels.append(lengthLabel)
        scriptCounts.append(scriptCount)
        scriptPerc = round(scriptCount * 100 / totalScriptCount, 1)
        scriptPercs.append(scriptPerc)

    bcode_df = pd.DataFrame({
        'BytecodeLength': lengthLabels,
        'ScriptCount': scriptCounts,
        'ScriptPerc': scriptPercs
    })

    return bcode_df


def analyzeBlockStats(stats_df, config, field):
    binFactor = config.blockBinFactor
    divisor = binFactor * 1000
    totalScriptCount = len(stats_df.index)

    blockLabels = []
    scriptCounts = []
    scriptPercs = []

    counter = Counter()

    for index, row in stats_df.iterrows():
        value = row[field]
        factor = int(value / divisor)
        counter[factor] += 1

    for factor, scriptCount in sorted(counter.items()):
        left = factor * binFactor
        right = left + binFactor
        blockLabel = '{left}K-{right}K'.format(left=left, right=right)
        blockLabels.append(blockLabel)
        scriptCounts.append(scriptCount)
        scriptPerc = round((scriptCount * 100) / totalScriptCount, 1)
        scriptPercs.append(scriptPerc)

    block_df = pd.DataFrame({
        field: blockLabels,
        'ScriptCount': scriptCounts,
        'ScriptPerc': scriptPercs
    })

    return block_df

def analyzeMaxBlockInstructionStats(stats_df, config):
    binFactor = config.blockInstructionBinFactor
    divisor = binFactor * 1000
    totalScriptCount = len(stats_df.index)

    blockLabels = []
    scriptCounts = []
    scriptPercs = []

    counter = Counter()

    for index, row in stats_df.iterrows():
        value = row['MaxBlockInstructionCount']
        factor = int(value / divisor)
        counter[factor] += 1

    for factor, scriptCount in sorted(counter.items()):
        left = factor * binFactor
        right = left + binFactor
        blockLabel = '{left}K-{right}K'.format(left=left, right=right)
        blockLabels.append(blockLabel)
        scriptCounts.append(scriptCount)
        scriptPerc = round((scriptCount * 100) / totalScriptCount, 1)
        scriptPercs.append(scriptPerc)

    block_df = pd.DataFrame({
        'MaxBlockInstructionCount': blockLabels,
        'ScriptCount': scriptCounts,
        'ScriptPerc': scriptPercs
    })

    return block_df

if __name__ == '__main__':
    config = getArgs()

    stats_df = readStats(config.statsFile)

    bcode_df = analyzeBytecodeStats(stats_df, config)
    print(bcode_df.to_markdown())

    block_df = analyzeBlockStats(stats_df, config, 'BlockPreOptCount')
    print(block_df.to_markdown())

    block_df = analyzeBlockStats(stats_df, config, 'BlockPostOptCount')
    print(block_df.to_markdown())

    block_df = analyzeMaxBlockInstructionStats(stats_df, config)
    print(block_df.to_markdown())
