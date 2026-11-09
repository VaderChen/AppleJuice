# 未定義分量沒有 reaching definition；保留原暫存器，不對空集合解參照或拆分。
x64arm_hlslcc_patch(UseDefineChains.cpp
  [=[                    AddDefineToList(defs, *(*ul)->psSiblings[j]->defines.begin());]=]
  [=[                    if (!(*ul)->psSiblings[j] || (*ul)->psSiblings[j]->defines.empty())
                        return 0;
                    AddDefineToList(defs, *(*ul)->psSiblings[j]->defines.begin());]=])
x64arm_hlslcc_patch(UseDefineChains.cpp
  [=[                ASSERT((*ul)->psSiblings[j] != NULL);
                ASSERT(!(*ul)->psSiblings[j]->defines.empty());]=]
  [=[                if (!(*ul)->psSiblings[j] || (*ul)->psSiblings[j]->defines.empty())
                    return 0;]=])

# Reachable 從 DE_def 開始。每個區塊的 VarKill 固定，前驅集合只增不減，
# 因此可直接累積新的定義，省掉重建整棵樹與配置已存在的節點。
x64arm_hlslcc_patch(ControlFlowGraph.cpp
  [=[    ReachableVariables newReachable = m_DEDef;
    bool hasChanges = false;

    // Loop each predecessor
    std::for_each(Preceding().begin(), Preceding().end(), [&](const Instruction *instr)
    {
        const BasicBlock *prec = m_Graph.GetBasicBlockForInstruction(instr);
        const ReachableVariables &precReachable = prec->Reachable();

        // Loop each variable*component
        std::for_each(precReachable.begin(), precReachable.end(), [&](const std::pair<uint32_t, BasicBlock::ReachableDefinitionsPerVariable> &itr2)
        {
            uint32_t regIdx = itr2.first;
            const BasicBlock::ReachableDefinitionsPerVariable &defs = itr2.second;

            // Already killed in this block?
            if (VarKill().find(regIdx) != VarKill().end())
                return;

            // Only do comparisons against current definitions if we've yet to find any changes
            BasicBlock::ReachableDefinitionsPerVariable *currReachablePerVar = 0;
            if (!hasChanges)
                currReachablePerVar = &m_Reachable[regIdx];

            BasicBlock::ReachableDefinitionsPerVariable &newReachablePerVar = newReachable[regIdx];

            // Loop each definition
            std::for_each(defs.begin(), defs.end(), [&](const BasicBlock::Definition &d)
            {
                if (!hasChanges)
                {
                    // Check if already there
                    if (currReachablePerVar->find(d) == currReachablePerVar->end())
                        hasChanges = true;
                }
                newReachablePerVar.insert(d);
            }); // definition
        }); // variable*component
    }); // predecessor

    if (hasChanges)
    {
        std::swap(m_Reachable, newReachable);
    }

    return hasChanges;]=]
  [=[    bool hasChanges = false;
    for (const Instruction *instr : Preceding())
    {
        const BasicBlock *prec = m_Graph.GetBasicBlockForInstruction(instr);
        if (prec == this)
            continue;

        for (const auto &entry : prec->Reachable())
        {
            if (VarKill().count(entry.first))
                continue;

            auto &reachable = m_Reachable[entry.first];
            for (const auto &definition : entry.second)
                hasChanges = reachable.insert(definition).second || hasChanges;
        }
    }
    return hasChanges;]=])
