// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Inc.

package com.starrocks.connector.jdbc;

import com.starrocks.catalog.Column;
import com.starrocks.catalog.JDBCTable;
import com.starrocks.catalog.PrimitiveType;
import com.starrocks.catalog.ScalarType;
import com.starrocks.catalog.Table;
import com.starrocks.catalog.Type;
import com.starrocks.common.DdlException;

import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Types;
import java.util.List;
import java.util.Map;

import static java.lang.Math.max;

public class PostgresSchemaResolver extends JDBCSchemaResolver {

    @Override
    public ResultSet getTables(Connection connection, String dbName) throws SQLException {
        return connection.getMetaData().getTables(connection.getCatalog(), dbName, null,
                new String[] {"TABLE", "VIEW", "MATERIALIZED VIEW", "FOREIGN TABLE"});
    }

    @Override
    public ResultSet getColumns(Connection connection, String dbName, String tblName) throws SQLException {
        return connection.getMetaData().getColumns(connection.getCatalog(), dbName, tblName, "%");
    }

    @Override
    public Table getTable(long id, String name, List<Column> schema, String dbName,
                          Map<String, String> properties) throws DdlException {
        return new JDBCTable(id, dbName + "." + name, schema, "", properties);
    }

    @Override
    public Type convertColumnType(int dataType, String typeName, int columnSize, int digits) {
        PrimitiveType primitiveType;
        switch (dataType) {
            case Types.BIT:
                primitiveType = PrimitiveType.BOOLEAN;
                break;
            case Types.SMALLINT:
                primitiveType = PrimitiveType.SMALLINT;
                break;
            case Types.INTEGER:
                primitiveType = PrimitiveType.INT;
                break;
            case Types.BIGINT:
                primitiveType = PrimitiveType.BIGINT;
                break;
            case Types.REAL:
                primitiveType = PrimitiveType.FLOAT;
                break;
            case Types.DOUBLE:
                primitiveType = PrimitiveType.DOUBLE;
                break;
            case Types.NUMERIC:
                primitiveType = PrimitiveType.DECIMAL32;
                break;
            case Types.CHAR:
                return ScalarType.createCharType(columnSize);
            case Types.VARCHAR:
                if (typeName.equalsIgnoreCase("varchar")) {
                    return ScalarType.createVarcharType(columnSize);
                } else if (typeName.equalsIgnoreCase("text")) {
                    return ScalarType.createVarcharType(ScalarType.MAX_VARCHAR_LENGTH);
                }
                primitiveType = PrimitiveType.UNKNOWN_TYPE;
                break;
            case Types.DATE:
                primitiveType = PrimitiveType.DATE;
                break;
            case Types.TIMESTAMP:
                primitiveType = PrimitiveType.DATETIME;
                break;
            default:
                primitiveType = PrimitiveType.UNKNOWN_TYPE;
                break;
        }

        if (primitiveType != PrimitiveType.DECIMAL32) {
            return ScalarType.createType(primitiveType);
        } else {
            int precision = columnSize + max(-digits, 0);
            return ScalarType.createUnifiedDecimalType(precision, max(digits, 0));
        }
    }

}
