// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Inc.

package com.starrocks.sql.ast;

import com.google.common.collect.Lists;
import com.google.common.collect.Maps;
import com.starrocks.analysis.DmlStmt;
import com.starrocks.analysis.Expr;
import com.starrocks.analysis.InsertTarget;
import com.starrocks.analysis.PartitionNames;
import com.starrocks.analysis.RedirectStatus;
import com.starrocks.analysis.TableName;
import com.starrocks.catalog.Column;
import com.starrocks.catalog.Table;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * Insert into is performed to load data from the result of query stmt.
 * <p>
 * syntax:
 * INSERT INTO table_name [partition_info] [col_list] [plan_hints] query_stmt
 * <p>
 * table_name: is the name of target table
 * partition_info: PARTITION (p1,p2)
 * the partition info of target table
 * col_list: (c1,c2)
 * the column list of target table
 * plan_hints: [STREAMING,SHUFFLE_HINT]
 * The streaming plan is used by both streaming and non-streaming insert stmt.
 * The only difference is that non-streaming will record the load info in LoadManager and return label.
 * User can check the load info by show load stmt.
 */
public class InsertStmt extends DmlStmt {
    public static final String SHUFFLE_HINT = "SHUFFLE";
    public static final String NOSHUFFLE_HINT = "NOSHUFFLE";
    public static final String STREAMING = "STREAMING";

    private final TableName tblName;
    private PartitionNames targetPartitionNames;
    // parsed from targetPartitionNames.
    // if targetPartitionNames is not set, add all formal partitions' id of the table into it
    private List<Long> targetPartitionIds = Lists.newArrayList();
    private final List<String> targetColumnNames;
    private QueryStatement queryStatement;
    private String label = null;

    // set after parse all columns and expr in query statement
    // this result expr in the order of target table's columns
    private final ArrayList<Expr> resultExprs = Lists.newArrayList();

    private final Map<String, Expr> exprByName = Maps.newTreeMap(String.CASE_INSENSITIVE_ORDER);

    private Table targetTable;

    private List<Column> targetColumns = Lists.newArrayList();
    private boolean isOverwrite;
    private long overwriteJobId = -1;

    // The default value of this variable is false, which means that the insert operation created by the user
    // it is not allowed to write data to the materialized view.
    // If this is set to true it means a system refresh operation, which is allowed to write to materialized view.
    private boolean isSystem = false;

    public InsertStmt(InsertTarget target, String label, List<String> cols, QueryStatement queryStatement,
                      List<String> hints, boolean isOverwrite) {
        this.tblName = target.getTblName();
        this.targetPartitionNames = target.getPartitionNames();
        this.label = label;
        this.queryStatement = queryStatement;
        this.targetColumnNames = cols;
        this.isOverwrite = isOverwrite;
    }

    // Ctor for CreateTableAsSelectStmt
    public InsertStmt(TableName name, QueryStatement queryStatement) {
        this.tblName = name;
        this.targetPartitionNames = null;
        this.targetColumnNames = null;
        this.queryStatement = queryStatement;
    }

    public Table getTargetTable() {
        return targetTable;
    }

    public void setTargetTable(Table targetTable) {
        this.targetTable = targetTable;
    }

    public String getDb() {
        return tblName.getDb();
    }

    public boolean isOverwrite() {
        return isOverwrite;
    }

    public void setOverwrite(boolean overwrite) {
        isOverwrite = overwrite;
    }

    public void setOverwriteJobId(long overwriteJobId) {
        this.overwriteJobId = overwriteJobId;
    }

    public boolean hasOverwriteJob() {
        return overwriteJobId > 0;
    }

    public QueryStatement getQueryStatement() {
        return queryStatement;
    }

    public void setQueryStatement(QueryStatement queryStatement) {
        this.queryStatement = queryStatement;
    }

    @Override
    public boolean isExplain() {
        return queryStatement.isExplain();
    }

    @Override
    public ExplainLevel getExplainLevel() {
        return queryStatement.getExplainLevel();
    }

    public String getLabel() {
        return label;
    }

    public boolean isSystem() {
        return isSystem;
    }

    public void setSystem(boolean system) {
        isSystem = system;
    }

    @Override
    public ArrayList<Expr> getResultExprs() {
        return resultExprs;
    }

    @Override
    public TableName getTableName() {
        return tblName;
    }

    public PartitionNames getTargetPartitionNames() {
        return targetPartitionNames;
    }

    public List<String> getTargetColumnNames() {
        return targetColumnNames;
    }

    public void setTargetPartitionNames(PartitionNames targetPartitionNames) {
        this.targetPartitionNames = targetPartitionNames;
    }

    public void setTargetPartitionIds(List<Long> targetPartitionIds) {
        this.targetPartitionIds = targetPartitionIds;
    }

    public List<Long> getTargetPartitionIds() {
        return targetPartitionIds;
    }

    public void setTargetColumns(List<Column> targetColumns) {
        this.targetColumns = targetColumns;
    }

    @Override
    public void reset() {
        super.reset();
        targetPartitionIds.clear();
        resultExprs.clear();
        exprByName.clear();
        targetColumns.clear();
    }

    @Override
    public RedirectStatus getRedirectStatus() {
        if (isExplain()) {
            return RedirectStatus.NO_FORWARD;
        } else {
            return RedirectStatus.FORWARD_WITH_SYNC;
        }
    }

    public <R, C> R accept(AstVisitor<R, C> visitor, C context) {
        return visitor.visitInsertStatement(this, context);
    }
}
