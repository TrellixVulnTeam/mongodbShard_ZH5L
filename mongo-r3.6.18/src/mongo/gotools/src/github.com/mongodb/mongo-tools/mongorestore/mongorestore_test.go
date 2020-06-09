// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongorestore

import (
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testtype"
	"github.com/mongodb/mongo-tools/common/testutil"
	"github.com/mongodb/mongo-tools/common/util"

	"os"
	"testing"

	. "github.com/smartystreets/goconvey/convey"
)

func init() {
	// bump up the verbosity to make checking debug log output possible
	log.SetVerbosity(&options.Verbosity{
		VLevel: 4,
	})
}

var (
	testServer = "localhost"
	testPort   = db.DefaultTestPort
)

func TestMongorestore(t *testing.T) {
	testtype.VerifyTestType(t, testtype.IntegrationTestType)
	_, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}

	Convey("With a test MongoRestore", t, func() {
		inputOptions := &InputOptions{}
		outputOptions := &OutputOptions{
			NumParallelCollections: 1,
			NumInsertionWorkers:    1,
		}
		nsOptions := &NSOptions{}
		provider, toolOpts, err := testutil.GetBareSessionProvider()
		if err != nil {
			log.Logvf(log.Always, "error connecting to host: %v", err)
			os.Exit(util.ExitError)
		}
		restore := MongoRestore{
			ToolOptions:     toolOpts,
			OutputOptions:   outputOptions,
			InputOptions:    inputOptions,
			NSOptions:       nsOptions,
			SessionProvider: provider,
		}
		session, _ := provider.GetSession()
		defer session.Close()
		c1 := session.DB("db1").C("c1")
		c1.DropCollection()
		Convey("and an explicit target restores from that dump directory", func() {
			restore.TargetDirectory = "testdata/testdirs"
			err = restore.Restore()
			So(err, ShouldBeNil)
			count, err := c1.Count()
			So(err, ShouldBeNil)
			So(count, ShouldEqual, 100)
		})

		Convey("and an target of '-' restores from standard input", func() {
			bsonFile, err := os.Open("testdata/testdirs/db1/c1.bson")
			restore.NSOptions.Collection = "c1"
			restore.NSOptions.DB = "db1"
			So(err, ShouldBeNil)
			restore.InputReader = bsonFile
			restore.TargetDirectory = "-"
			err = restore.Restore()
			So(err, ShouldBeNil)
			count, err := c1.Count()
			So(err, ShouldBeNil)
			So(count, ShouldEqual, 100)
		})

	})
}

func TestRestoreUsersOrRoles(t *testing.T) {
	testtype.VerifyTestType(t, testtype.IntegrationTestType)

	nsOptions := &NSOptions{}
	provider, toolOpts, err := testutil.GetBareSessionProvider()
	if err != nil {
		log.Logvf(log.Always, "error connecting to host: %v", err)
		os.Exit(util.ExitError)
	}

	Convey("With a test MongoRestore", t, func() {
		inputOptions := &InputOptions{}
		outputOptions := &OutputOptions{
			NumParallelCollections: 1,
			NumInsertionWorkers:    1,
			TempUsersColl:          "tempusers",
			TempRolesColl:          "temproles",
		}
		restore := MongoRestore{
			ToolOptions:     toolOpts,
			OutputOptions:   outputOptions,
			InputOptions:    inputOptions,
			NSOptions:       nsOptions,
			SessionProvider: provider,
		}
		session, _ := provider.GetSession()
		defer session.Close()

		db := session.DB("admin")

		Convey("Restoring users and roles should drop tempusers and temproles", func() {
			restore.TargetDirectory = "testdata/usersdump"
			err := restore.Restore()
			So(err, ShouldBeNil)

			adminCollections, err := db.CollectionNames()
			So(err, ShouldBeNil)

			for _, collName := range adminCollections {
				So(collName, ShouldNotEqual, "tempusers")
				So(collName, ShouldNotEqual, "temproles")
			}
		})
	})
}

func TestKnownCollections(t *testing.T) {
	testtype.VerifyTestType(t, testtype.IntegrationTestType)

	Convey("With a test MongoRestore", t, func() {
		inputOptions := &InputOptions{}
		outputOptions := &OutputOptions{
			NumParallelCollections: 1,
			NumInsertionWorkers:    1,
		}
		nsOptions := &NSOptions{}
		provider, toolOpts, err := testutil.GetBareSessionProvider()
		if err != nil {
			log.Logvf(log.Always, "error connecting to host: %v", err)
			os.Exit(util.ExitError)
		}
		restore := MongoRestore{
			ToolOptions:     toolOpts,
			OutputOptions:   outputOptions,
			InputOptions:    inputOptions,
			NSOptions:       nsOptions,
			SessionProvider: provider,
		}
		session, _ := provider.GetSession()
		defer session.Close()

		db := session.DB("test")
		defer func() {
			db.C("foo").DropCollection()
		}()

		Convey("Once collection foo has been restored, it should exist in restore.knownCollections", func() {
			restore.TargetDirectory = "testdata/foodump"
			err := restore.Restore()
			So(err, ShouldBeNil)

			var namespaceExistsInCache bool
			if cols, ok := restore.knownCollections["test"]; ok {
				for _, collName := range cols {
					if collName == "foo" {
						namespaceExistsInCache = true
					}
				}
			}
			So(namespaceExistsInCache, ShouldBeTrue)
		})
	})
}
