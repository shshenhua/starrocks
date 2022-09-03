// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Inc.

package com.starrocks.sql.optimizer.dump;

import com.google.common.base.Preconditions;
import com.google.common.collect.Maps;
import com.starrocks.catalog.Resource;
import com.starrocks.catalog.Table;
import com.starrocks.catalog.View;
import com.starrocks.common.Pair;
import com.starrocks.qe.SessionVariable;
import com.starrocks.qe.VariableMgr;
import com.starrocks.sql.optimizer.statistics.ColumnStatistic;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

public class QueryDumpInfo implements DumpInfo {
    private String originStmt = "";
    private final Set<Resource> resourceSet = new HashSet<>();
    // tableId-><dbName, table>
    private final Map<Long, Pair<String, Table>> tableMap = new HashMap<>();
    // resourceName->dbName->tableName->externalTable
    private final Map<String, Map<String, Map<String, HiveMetaStoreTableDumpInfo>>> hmsTableMap = new HashMap<>();
    // viewId-><dbName, view>
    private final Map<Long, Pair<String, View>> viewMap = new LinkedHashMap<>();
    // tableName->partitionName->partitionRowCount
    private final Map<String, Map<String, Long>> partitionRowCountMap = new HashMap<>();
    // tableName->columnName->column statistics
    private final Map<String, Map<String, ColumnStatistic>> tableStatisticsMap = new HashMap<>();
    private SessionVariable sessionVariable;
    // tableName->createTableStmt
    private final Map<String, String> createTableStmtMap = new HashMap<>();
    // viewName->createViewStmt
    private final Map<String, String> createViewStmtMap = new LinkedHashMap<>();

    private final List<String> createResourceStmtList = new ArrayList<>();

    private final List<String> exceptionList = new ArrayList<>();
    private int beNum;
    private int cachedAvgNumOfHardwareCores = -1;
    private Map<Long, Integer> numOfHardwareCoresPerBe = Maps.newHashMap();

    public QueryDumpInfo(SessionVariable sessionVariable) {
        this.sessionVariable = sessionVariable;
    }

    public QueryDumpInfo() {
        this.sessionVariable = VariableMgr.newSessionVariable();
    }

    @Override
    public void setOriginStmt(String stmt) {
        originStmt = stmt;
    }

    public String getOriginStmt() {
        return originStmt;
    }

    public SessionVariable getSessionVariable() {
        return sessionVariable;
    }

    public void setSessionVariable(SessionVariable sessionVariable) {
        this.sessionVariable = sessionVariable;
    }

    @Override
    public void addTable(String dbName, Table table) {
        tableMap.put(table.getId(), new Pair<>(dbName, table));
    }

    @Override
    public void addPartitionRowCount(Table table, String partition, long rowCount) {
        String tableName = getTableName(table.getId());
        addPartitionRowCount(tableName, partition, rowCount);
    }

    public void setCachedAvgNumOfHardwareCores(int cores) {
        cachedAvgNumOfHardwareCores = cores;
    }

    public int getCachedAvgNumOfHardwareCores() {
        return this.cachedAvgNumOfHardwareCores;
    }

    public void addNumOfHardwareCoresPerBe(Map<Long, Integer> numOfHardwareCoresPerBe) {
        this.numOfHardwareCoresPerBe.putAll(numOfHardwareCoresPerBe);
    }

    public Map<Long, Integer> getNumOfHardwareCoresPerBe() {
        return this.numOfHardwareCoresPerBe;
    }

    @Override
    public void addResource(Resource resource) {
        resourceSet.add(resource);
    }

    public Set<Resource> getResourceSet() {
        return resourceSet;
    }

    @Override
    public void addView(String dbName, View view) {
        viewMap.put(view.getId(), new Pair<>(dbName, view));
    }

    @Override
    public void reset() {
        this.originStmt = "";
        this.tableMap.clear();
        this.partitionRowCountMap.clear();
        this.tableStatisticsMap.clear();
        this.createTableStmtMap.clear();
        this.numOfHardwareCoresPerBe.clear();
        this.resourceSet.clear();
        this.hmsTableMap.clear();
        this.exceptionList.clear();
    }

    public void addPartitionRowCount(String tableName, String partition, long rowCount) {
        if (!partitionRowCountMap.containsKey(tableName)) {
            partitionRowCountMap.put(tableName, new HashMap<>());
        }
        partitionRowCountMap.get(tableName).put(partition, rowCount);
    }

    public Map<String, Map<String, Long>> getPartitionRowCountMap() {
        return partitionRowCountMap;
    }

    public HiveMetaStoreTableDumpInfo getHMSTable(String resourceName, String dbName, String tableName) {
        return hmsTableMap.get(resourceName).get(dbName).get(tableName);
    }

    @Override
    public Map<String, Map<String, Map<String, HiveMetaStoreTableDumpInfo>>> getHmsTableMap() {
        return hmsTableMap;
    }

    @Override
    public void addHMSTable(String resourceName, String dbName, String tableName) {
        addHMSTable(resourceName, dbName, tableName, new HiveTableDumpInfo());
    }

    public void addHMSTable(String resourceName, String dbName, String tableName, HiveMetaStoreTableDumpInfo dumpInfo) {
        hmsTableMap.putIfAbsent(resourceName, new HashMap<>());
        Map<String, Map<String, HiveMetaStoreTableDumpInfo>> dbTable = hmsTableMap.get(resourceName);
        dbTable.putIfAbsent(dbName, new HashMap<>());
        Map<String, HiveMetaStoreTableDumpInfo> tableMap =  dbTable.get(dbName);
        tableMap.putIfAbsent(tableName, dumpInfo);
    }

    @Override
    public void addTableStatistics(Table table, String column, ColumnStatistic columnStatistic) {
        addTableStatistics(getTableName(table.getId()), column, columnStatistic);
    }

    public void addTableStatistics(String tableName, String column, ColumnStatistic columnStatistic) {
        if (!tableStatisticsMap.containsKey(tableName)) {
            tableStatisticsMap.put(tableName, new HashMap<>());
        }
        tableStatisticsMap.get(tableName).put(column, columnStatistic);
    }

    public Map<String, Map<String, ColumnStatistic>> getTableStatisticsMap() {
        return tableStatisticsMap;
    }

    public Map<Long, Pair<String, Table>> getTableMap() {
        return tableMap;
    }

    public Map<Long, Pair<String, View>> getViewMap() {
        return viewMap;
    }

    public Map<String, String> getCreateTableStmtMap() {
        return createTableStmtMap;
    }

    public Map<String, String> getCreateViewStmtMap() {
        return createViewStmtMap;
    }

    // return table full name
    public String getTableName(long tableId) {
        Table table = tableMap.get(tableId).second;
        Preconditions.checkState(table != null);
        return tableMap.get(tableId).first + "." + tableMap.get(tableId).second.getName();
    }

    public void addTableCreateStmt(String tableName, String createTableStmt) {
        createTableStmtMap.put(tableName, createTableStmt);
    }

    public void addViewCreateStmt(String viewName, String createViewStmt) {
        createViewStmtMap.put(viewName, createViewStmt);
    }

    public void addResourceCreateStmt(String resourceCreateStmt) {
        createResourceStmtList.add(resourceCreateStmt);
    }

    public List<String> getCreateResourceStmtList() {
        return createResourceStmtList;
    }

    @Override
    public void addException(String exception) {
        this.exceptionList.add(exception);
    }

    public List<String> getExceptionList() {
        return this.exceptionList;
    }

    public void setBeNum(int beNum) {
        this.beNum = beNum;
    }

    public int getBeNum() {
        return this.beNum;
    }
}
