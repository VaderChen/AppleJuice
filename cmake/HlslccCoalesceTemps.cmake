# CoalesceTemps 原本對每條指令、每個新暫存器（OrigTemps..TotalTemps）、每個運算元各遞迴一次 ChangeOperandTempRegister，
# 大型著色器是指令數×新暫存器數的平方級成本（DQ3 讀檔時段主執行緒約 20% 在此）。改為每個運算元走訪一次，
# 新暫存器號加上 0x10000；與原呼叫（完整遮罩、UD_CHANGE_ALL、不 rebase）語義相同。
x64arm_hlslcc_patch(Shader.cpp
  [=[// Coalesce the split temps back based on their original temp register. Keep uint/int/float operations separate
static void CoalesceTemps(]=]
  [=[// 新暫存器一次搬到 0x10000 以上：遞迴子運算元，與逐一呼叫 ChangeOperandTempRegister(i, 0x10000 + i, ALL, UD_CHANGE_ALL, 0) 等價。
static void RebaseNewTemps(Operand *psOperand, uint32_t first, uint32_t last)
{
    for (uint32_t s = 0; s < MAX_SUB_OPERANDS; s++)
        if (psOperand->m_SubOperands[s].get())
            RebaseNewTemps(psOperand->m_SubOperands[s].get(), first, last);
    if (psOperand->eType == OPERAND_TYPE_TEMP && psOperand->ui32RegisterNumber >= first && psOperand->ui32RegisterNumber < last)
        psOperand->ui32RegisterNumber += 0x10000;
}

// 收集運算元樹（含子運算元）中出現的暫存器號，不重複。
static void CollectTempRegisters(const Operand *psOperand, std::vector<uint32_t> &regs)
{
    for (uint32_t s = 0; s < MAX_SUB_OPERANDS; s++)
        if (psOperand->m_SubOperands[s].get())
            CollectTempRegisters(psOperand->m_SubOperands[s].get(), regs);
    if (psOperand->eType == OPERAND_TYPE_TEMP && std::find(regs.begin(), regs.end(), psOperand->ui32RegisterNumber) == regs.end())
        regs.push_back(psOperand->ui32RegisterNumber);
}

// Coalesce the split temps back based on their original temp register. Keep uint/int/float operations separate
static void CoalesceTemps(]=])
x64arm_hlslcc_patch(Shader.cpp
  [=[        // Update all operands and their suboperands
        for (i = psPhase->ui32OrigTemps; i < psPhase->ui32TotalTemps; i++)
        {
            for (k = 0; k < inst->ui32NumOperands; k++)
                inst->ChangeOperandTempRegister(&inst->asOperands[k], i, 0x10000 + i, OPERAND_4_COMPONENT_MASK_ALL, UD_CHANGE_ALL, 0);
        }
        inst++;]=]
  [=[        // Update all operands and their suboperands
        for (k = 0; k < inst->ui32NumOperands; k++)
            RebaseNewTemps(&inst->asOperands[k], psPhase->ui32OrigTemps, psPhase->ui32TotalTemps);
        inst++;]=])
# 第二階段：原本每個要搬移的暫存器都重掃全部指令（同樣是暫存器數×指令數）。改為先記錄每個暫存器的目標與 rebase，
# 迴圈結束後一次走訪全部指令，對運算元樹中出現的每個暫存器套用同一個 ChangeOperandTempRegister。原暫存器搬到
# MaxOrigTemps 以上、分割暫存器（0x10000+i）搬到原暫存器號，搬移之間不會串接，延後套用與逐一套用結果相同。
x64arm_hlslcc_patch(Shader.cpp
  [=[    for (i = 0; i < psPhase->ui32TotalTemps; i++)
    {
        uint32_t rebase, count;
        uint32_t newReg = i;
        uint32_t origReg = i;
        int needsMoving = 0;]=]
  [=[    struct TempMove { uint32_t newReg, rebase; bool valid; };
    std::vector<TempMove> moves(psPhase->ui32TotalTemps, TempMove{0, 0, false});
    for (i = 0; i < psPhase->ui32TotalTemps; i++)
    {
        uint32_t rebase, count;
        uint32_t newReg = i;
        uint32_t origReg = i;
        int needsMoving = 0;]=])
x64arm_hlslcc_patch(Shader.cpp
  [=[        if (needsMoving)
        {
            //          printf("Moving reg %d to %d, count %d rebase %d\n", origReg, newReg, count, rebase);

            // Move directly to correct location
            inst = &psPhase->psInst[0];
            while (inst <= psLastInstruction)
            {
                for (k = 0; k < inst->ui32NumOperands; k++)
                    inst->ChangeOperandTempRegister(&inst->asOperands[k], origReg, newReg, OPERAND_4_COMPONENT_MASK_ALL, UD_CHANGE_ALL, rebase);
                inst++;
            }
        }]=]
  [=[        if (needsMoving)
            moves[i] = TempMove{newReg, rebase, true}; // 迴圈結束後一次套用（origReg 由 i 推得）。
        (void)origReg;]=])
x64arm_hlslcc_patch(Shader.cpp
  [=[                psShader->psDoubleTempSizes[newReg] = std::max(psShader->psDoubleTempSizes[newReg], (char)count);
                break;
        }
    }
}]=]
  [=[                psShader->psDoubleTempSizes[newReg] = std::max(psShader->psDoubleTempSizes[newReg], (char)count);
                break;
        }
    }
    // 一次走訪套用全部搬移：原暫存器 i（< OrigTemps）與分割暫存器 0x10000+i 各自查表。
    const auto lookup = [&](uint32_t reg) -> const TempMove * {
        const uint32_t index = reg >= 0x10000 ? reg - 0x10000 : reg;
        if (index >= moves.size() || !moves[index].valid)
            return nullptr;
        if ((reg >= 0x10000) != (index >= psPhase->ui32OrigTemps))
            return nullptr;
        return &moves[index];
    };
    std::vector<uint32_t> regs;
    inst = &psPhase->psInst[0];
    while (inst <= psLastInstruction)
    {
        for (k = 0; k < inst->ui32NumOperands; k++)
        {
            regs.clear();
            CollectTempRegisters(&inst->asOperands[k], regs);
            for (const auto reg : regs)
                if (const auto *move = lookup(reg))
                    inst->ChangeOperandTempRegister(&inst->asOperands[k], reg, move->newReg, OPERAND_4_COMPONENT_MASK_ALL, UD_CHANGE_ALL, move->rebase);
        }
        inst++;
    }
}]=])
