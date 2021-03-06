/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution. 
 *
 * Redistribution and use in source and binary forms, with or without modification, 
 * are permitted provided that the following conditions are met: 
 *
 * - Redistributions of source code must retain the above copyright notice, 
 *   this list of conditions and the following disclaimer. 
 *
 * - Redistributions in binary form must reproduce the above copyright notice, 
 *   this list of conditions and the following disclaimer in the documentation 
 *   and/or other materials provided with the distribution. 
 *
 * - Neither the name of the <ORGANIZATION> nor the names of its contributors 
 *   may be used to endorse or promote products derived from this software without 
 *   specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND 
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 
 * IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, 
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, 
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY 
 * OF SUCH DAMAGE. 
 *
 */

package rye.jdbc.driver;

import java.sql.Array;
import java.sql.Blob;
import java.sql.CallableStatement;
import java.sql.Clob;
import java.sql.Connection;
import java.sql.DatabaseMetaData;
import java.sql.NClob;
import java.sql.ResultSet;
import java.sql.SQLClientInfoException;
import java.sql.SQLException;
import java.sql.SQLWarning;
import java.sql.SQLXML;
import java.sql.Savepoint;
import java.sql.Statement;
import java.sql.Struct;
import java.sql.SQLFeatureNotSupportedException;
import java.util.ArrayList;
import java.util.Map;
import java.util.Properties;
import java.util.concurrent.Executor;

import rye.jdbc.jci.JciConnection;
import rye.jdbc.jci.JciStatement;
import rye.jdbc.sharding.ShardAdmin;
import rye.jdbc.sharding.ShardNodeInstance;

public class RyeConnection implements Connection
{
    JciConnection jciCon;
    String user;
    String url;

    protected boolean is_closed;
    private boolean auto_commit;
    protected int holdability;

    private boolean ending;

    private ArrayList<RyeStatement> statements;
    private RyeDatabaseMetaData mdata;

    protected RyeConnection(JciConnection con, String url, String user)
    {
	this.jciCon = con;
	this.jciCon.setRyeConnection(this);
	this.url = url;
	this.user = user;
	this.is_closed = false;
	this.auto_commit = true;
	this.holdability = ResultSet.HOLD_CURSORS_OVER_COMMIT;
	this.mdata = null;
	this.ending = false;
	this.statements = new ArrayList<RyeStatement>();
    }

    /*
     * java.sql.Connection interface
     */

    @Override
    public String toString()
    {
	StringBuffer str = new StringBuffer();
	str.append(getClass().getName());
	if (is_closed) {
	    str.append("-closed");
	}
	else {
	    str.append(String.format("(%s)", jciCon.getCasInfoString()));
	}
	return str.toString();
    }

    public synchronized RyeStatement createStatement() throws SQLException
    {
	return createStatement(ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, holdability);
    }

    public synchronized RyeStatement createStatement(int resultSetType, int resultSetConcurrency) throws SQLException
    {
	return createStatement(resultSetType, resultSetConcurrency, holdability);
    }

    public synchronized RyeStatement createStatement(int type, int concur, int holdable) throws SQLException
    {
	checkIsOpen();

	checkStatementType(type, concur, holdable);

	RyeStatement stmt = new RyeStatement(this, type, holdable);

	addStatement(stmt);

	return stmt;
    }

    public synchronized RyePreparedStatement prepareStatement(String sql) throws SQLException
    {
	return prepareStatement(sql, ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, holdability);
    }

    public synchronized RyePreparedStatement prepareStatement(String sql, int resultSetType, int resultSetConcurrency)
		    throws SQLException
    {
	return prepareStatement(sql, resultSetType, resultSetConcurrency, holdability);
    }

    public synchronized RyePreparedStatement prepareStatement(String sql, int autoGeneratedKeys) throws SQLException
    {
	if (autoGeneratedKeys == Statement.RETURN_GENERATED_KEYS) {
	    throw new SQLFeatureNotSupportedException();
	}

	return prepareStatement(sql, ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, holdability);
    }

    public synchronized RyePreparedStatement prepareStatement(String sql, int type, int concur, int holdable)
		    throws SQLException
    {
	checkIsOpen();

	checkStatementType(type, concur, holdable);

	byte prepareFlag = (byte) 0;

	if (holdable == ResultSet.HOLD_CURSORS_OVER_COMMIT && jciCon.supportHoldableResult()) {
	    prepareFlag = JciConnection.getPrepareFlag(true);
	}

	JciStatement us = prepare(sql, prepareFlag);

	RyePreparedStatement pstmt = new RyePreparedStatement(this, us, type, concur, holdable);

	addStatement(pstmt);

	return pstmt;
    }

    public synchronized RyePreparedStatement prepareStatement(String sql, int[] indexes) throws SQLException
    {
	throw new SQLFeatureNotSupportedException();
    }

    public synchronized RyePreparedStatement prepareStatement(String sql, String[] colName) throws SQLException
    {
	throw new SQLFeatureNotSupportedException();
    }

    public synchronized CallableStatement prepareCall(String sql) throws SQLException
    {
	throw new SQLFeatureNotSupportedException();
    }

    public CallableStatement prepareCall(String sql, int resultSetType, int resultSetConcurrency) throws SQLException
    {
	throw new SQLFeatureNotSupportedException();
    }

    public CallableStatement prepareCall(String sql, int type, int concur, int holdable) throws SQLException
    {
	throw new SQLFeatureNotSupportedException();
    }

    public String nativeSQL(String sql) throws SQLException
    {
	throw new UnsupportedOperationException();
    }

    public synchronized void setAutoCommit(boolean autoCommit) throws SQLException
    {
	checkIsOpen();

	if (auto_commit != autoCommit)
	    commit();
	auto_commit = autoCommit;
	jciCon.setAutoCommit(autoCommit);
    }

    public synchronized boolean getAutoCommit() throws SQLException
    {
	checkIsOpen();
	return auto_commit;
    }

    public synchronized void commit() throws SQLException
    {
	checkIsOpen();

	if (ending)
	    return;
	ending = true;

	completeStatementForCommit();

	try {
	    jciCon.endTranRequest(true);
	} finally {
	    ending = false;
	}

	ending = false;
    }

    public synchronized void rollback() throws SQLException
    {
	checkIsOpen();

	if (ending)
	    return;
	ending = true;

	completeAllStatements();

	try {
	    jciCon.endTranRequest(false);
	} finally {
	    ending = false;
	}

	ending = false;
    }

    public synchronized void close() throws SQLException
    {
	if (is_closed)
	    return;

	clear();

	is_closed = true;

	jciCon.close();

	jciCon = null;
	url = null;
	user = null;
	mdata = null;
	statements = null;
    }

    public synchronized boolean isClosed() throws SQLException
    {
	return is_closed;
    }

    public synchronized DatabaseMetaData getMetaData() throws SQLException
    {
	checkIsOpen();

	if (mdata != null)
	    return mdata;

	mdata = new RyeDatabaseMetaData(this);
	return mdata;
    }

    public synchronized void setReadOnly(boolean readOnly) throws SQLException
    {
	checkIsOpen();
    }

    public synchronized boolean isReadOnly() throws SQLException
    {
	checkIsOpen();
	return false;
    }

    public synchronized void setCatalog(String catalog) throws SQLException
    {
	checkIsOpen();
    }

    public synchronized String getCatalog() throws SQLException
    {
	checkIsOpen();
	return "";
    }

    public synchronized void setTransactionIsolation(int level) throws SQLException
    {
	checkIsOpen();

	if (level != TRANSACTION_READ_UNCOMMITTED) {
	    throw createRyeException(RyeErrorCode.ER_INVALID_TRAN_ISOLATION_LEVEL, null);
	}
    }

    public synchronized int getTransactionIsolation() throws SQLException
    {
	checkIsOpen();

	return TRANSACTION_READ_UNCOMMITTED;
    }

    public synchronized SQLWarning getWarnings() throws SQLException
    {
	checkIsOpen();
	return null;
    }

    public synchronized void clearWarnings() throws SQLException
    {
	checkIsOpen();
    }

    public Map<String, Class<?>> getTypeMap() throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public void setTypeMap(Map<String, Class<?>> map) throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public synchronized int getHoldability() throws SQLException
    {
	checkIsOpen();

	return holdability;
    }

    public synchronized void releaseSavepoint(Savepoint savepoint) throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public synchronized void rollback(Savepoint savepoint) throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public synchronized void setHoldability(int holdable) throws SQLException
    {
	checkIsOpen();

	holdability = holdable;
    }

    public synchronized Savepoint setSavepoint() throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public synchronized Savepoint setSavepoint(String name) throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public Blob createBlob() throws SQLException
    {
	throw new SQLFeatureNotSupportedException();
    }

    public Clob createClob() throws SQLException
    {
	throw new SQLFeatureNotSupportedException();
    }

    public NClob createNClob() throws SQLException
    {
	throw new SQLFeatureNotSupportedException();
    }

    public Array createArrayOf(String arg0, Object[] arg1) throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public SQLXML createSQLXML() throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public Struct createStruct(String arg0, Object[] arg1) throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public Properties getClientInfo() throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public String getClientInfo(String arg0) throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public boolean isValid(int arg0) throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public void setClientInfo(Properties arg0) throws SQLClientInfoException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public void setClientInfo(String arg0, String arg1) throws SQLClientInfoException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public boolean isWrapperFor(Class<?> arg0) throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public <T> T unwrap(Class<T> arg0) throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public void setSchema(String schema) throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public String getSchema() throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public void abort(Executor executor) throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public void setNetworkTimeout(Executor executor, int milliseconds) throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    public int getNetworkTimeout() throws SQLException
    {
	throw new java.lang.UnsupportedOperationException();
    }

    /*
     * RyeConnection methods
     */

    public void cancel() throws SQLException
    {
	checkIsOpen();
	jciCon.cancelRequest();
    }

    public synchronized JciConnection getJciConnection() throws SQLException
    {
	checkIsOpen();
	return jciCon;
    }

    JciStatement prepare(String sql, byte prepareFlag) throws SQLException
    {
	try {
	    return jciCon.prepareRequest(sql, prepareFlag);
	} catch (SQLException e) {
	    autoRollback();
	    throw e;
	}
    }

    void autoCommit() throws SQLException
    {
	checkIsOpen();
	if (auto_commit)
	    commit();
    }

    void autoRollback() throws SQLException
    {
	checkIsOpen();
	if (auto_commit)
	    rollback();
    }

    synchronized void closeConnection() throws SQLException
    {
	if (is_closed)
	    return;

	clear();
	is_closed = true;
    }

    synchronized void removeStatement(Statement s) throws SQLException
    {
	int i = statements.indexOf(s);
	if (i > -1)
	    statements.remove(i);
    }

    private void clear() throws SQLException
    {
	closeAllStatements();
    }

    private void checkIsOpen() throws SQLException
    {
	if (is_closed) {
	    throw createRyeException(RyeErrorCode.ER_CONNECTION_CLOSED, null);
	}
    }

    private void addStatement(RyeStatement s) throws SQLException
    {
	statements.add(s);

	if (jciCon.getQueryTimeout() >= 0) {
	    s.setQueryTimeout(jciCon.getQueryTimeout());
	}
    }

    private void completeStatementForCommit() throws SQLException
    {
	for (int i = 0; i < statements.size(); i++) {
	    RyeStatement stmt = (RyeStatement) statements.get(i);

	    if (stmt.getHoldability() == ResultSet.HOLD_CURSORS_OVER_COMMIT) {
		stmt.setCurrentTransaction(false);
		continue;
	    }
	    else if (stmt instanceof RyePreparedStatement) {
		statements.remove(i);
		if (jciCon.brokerInfoStatementPooling() == true)
		    stmt.complete();
		else
		    stmt.close();
	    }
	    else
		stmt.complete();
	}
    }

    private void completeAllStatements() throws SQLException
    {
	for (int i = 0; i < statements.size(); i++) {
	    RyeStatement stmt = (RyeStatement) statements.get(i);

	    if (stmt.getHoldability() == ResultSet.HOLD_CURSORS_OVER_COMMIT && !stmt.isFromCurrentTransaction()) {
		continue;
	    }

	    if (stmt instanceof RyePreparedStatement) {

		statements.remove(i);
		if (jciCon.brokerInfoStatementPooling() == true)
		    stmt.complete();
		else
		    stmt.close();
	    }
	    else
		stmt.complete();
	}
    }

    private void closeAllStatements() throws SQLException
    {
	Object stmts[] = statements.toArray();
	for (int i = 0; i < stmts.length; i++) {
	    RyeStatement stmt = (RyeStatement) stmts[i];
	    stmt.close();
	}
	statements.clear();
    }

    protected void checkStatementType(int type, int concur, int holdable) throws SQLException
    {
	if (type != ResultSet.TYPE_FORWARD_ONLY || concur != ResultSet.CONCUR_READ_ONLY) {
	    throw new SQLFeatureNotSupportedException();
	}
    }

    protected void finalize()
    {
	try {
	    close();
	} catch (Exception e) {
	}
    }

    RyeException createRyeException(int errCode, Throwable t)
    {
	return RyeException.createRyeException(jciCon, errCode, t);
    }

    RyeException createRyeException(int errCode, String msg, Throwable t)
    {
	return RyeException.createRyeException(jciCon, errCode, msg, t);
    }

    public void setTraceShardConnection(int level)
    {
	jciCon.setTraceShardConnection(level);
    }

    public String getTraceShardConnection()
    {
	return jciCon.getTraceShardConnection();
    }

    public ShardAdmin getShardAdmin()
    {
	return jciCon.getShardAdmin();
    }

    public void setReuseShardStatement(boolean reuseStatement)
    {
	jciCon.setReuseShardStatement(reuseStatement);
    }

    public String getDatabaseName()
    {
	return jciCon.getDatabaseName();
    }

    public String getHostname()
    {
	return jciCon.getHostname();
    }

    public boolean isShardingConnection()
    {
	return jciCon.isShardingConnection();
    }

    public Object[] checkShardNodes() throws RyeException
    {
	return jciCon.checkShardNodes();
    }

    public int getServerHaMode() throws RyeException
    {
	return jciCon.getServerHaMode();
    }

    public short getStatusInfoServerNodeid()
    {
	return jciCon.getStatusInfoServerNodeid();
    }

    public ShardNodeInstance[] getShardNodeInstance()
    {
	return jciCon.getShardNodeInstance();
    }

    public int getServerStartTime()
    {
	return jciCon.getServerStartTime();
    }
}
